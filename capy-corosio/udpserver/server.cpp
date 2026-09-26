/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * One socket, per client only its address and progress: on each request
 * datagram the whole file is streamed back to the requester's address as
 * fixed-size datagrams (DGRAM_BYTES, default the max IPv4 UDP payload, 65507
 * bytes), followed by a zero-length datagram -- the end-of-transfer sentinel,
 * since UDP has no end-of-stream of its own.
 *
 * Clients are served concurrently, interleaved one datagram at a time.
 * Corosio's UDP socket allows only one send_to() and one recv_from() in
 * flight at once ("A socket must not have concurrent operations of the same
 * type"; the socket keeps a single slot per operation type), so one coroutine
 * per client would not do, even on a strand: two of them could each have a
 * send_to() pending. Instead one receive loop records each request as an
 * active transfer, and one send loop walks the active transfers round-robin,
 * sending the next datagram of each (or its sentinel, which ends it). The two
 * loops may run on different threads; a mutex guards the list of transfers,
 * never held across an await.
 *
 * Deliberately uses the OS's default socket buffer sizes, like every other
 * scenario: any loss it causes at these datagram sizes is a real, comparable
 * measurement, not an artifact of tuning some implementations and not others.
 */

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;
// 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
// payload a single send_to() can carry; anything above it fails.
constexpr std::size_t DEFAULT_DGRAM_BYTES = 65507;

struct FileMapping {
    int fd = -1;
    const char* data = nullptr;
    std::size_t size = 0;
};

static FileMapping map_file_read_only(const fs::path& path) {
    FileMapping mapping{};

    const std::uintmax_t file_size = fs::file_size(path);
    mapping.size = static_cast<std::size_t>(file_size);

    mapping.fd = open(path.c_str(), O_RDONLY);
    if (mapping.fd == -1) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    if (mapping.size == 0) {
        return mapping;
    }

    void* ptr = mmap(nullptr, mapping.size, PROT_READ, MAP_PRIVATE, mapping.fd, 0);
    if (ptr == MAP_FAILED) {
        close(mapping.fd);
        throw std::runtime_error("mmap failed.");
    }

    mapping.data = static_cast<const char*>(ptr);
    return mapping;
}

static void unmap_file(FileMapping& mapping) {
    if (mapping.data != nullptr && mapping.size > 0) {
        munmap(const_cast<char*>(mapping.data), mapping.size);
    }
    if (mapping.fd != -1) {
        close(mapping.fd);
    }
    mapping.data = nullptr;
    mapping.fd = -1;
    mapping.size = 0;
}

static std::size_t dgram_bytes() {
    if (const char* s = std::getenv("DGRAM_BYTES")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v > 0) {
            return static_cast<std::size_t>(v);
        }
    }
    return DEFAULT_DGRAM_BYTES;
}

// One client's transfer in progress: where to send and how much has been sent.
struct Transfer {
    corosio::endpoint client;
    std::size_t sent = 0;
};

// State shared by the receive and send loops.
struct Server {
    corosio::udp_socket& sock;
    corosio::io_context::executor_type ex;
    std::mutex mutex;       // guards active and sending
    std::span<const char> payload;
    std::size_t dgram_size;
    std::vector<Transfer> active;
    bool sending = false;   // send_loop is running
};

// Sends one datagram per active transfer in turn until none is left. A
// transfer ends after its zero-length sentinel, or when a send fails.
static capy::task<void> send_loop(Server& s) {
    std::size_t next = 0;
    for (;;) {
        corosio::endpoint client;
        std::size_t offset;
        {
            std::lock_guard lock(s.mutex);
            if (s.active.empty()) {
                s.sending = false;
                co_return;
            }
            if (next >= s.active.size()) {
                next = 0;
            }
            client = s.active[next].client;
            offset = s.active[next].sent;
        }
        const std::size_t n = std::min(s.dgram_size, s.payload.size() - offset);
        auto [ec, sn] = co_await s.sock.send_to(
            capy::const_buffer(s.payload.data() + offset, n), client);
        std::lock_guard lock(s.mutex);
        if (n == 0 || ec || sn == 0) {
            s.active.erase(s.active.begin() + static_cast<std::ptrdiff_t>(next));
            continue;
        }
        s.active[next].sent += static_cast<std::size_t>(sn);
        ++next;
    }
}

static capy::task<void> serve_loop(Server& s) {
    std::array<char, 64> request{};
    for (;;) {
        corosio::endpoint client;
        auto [ec, n] = co_await s.sock.recv_from(
            capy::mutable_buffer(request.data(), request.size()), client);
        if (ec) {
            continue;
        }
        bool start = false;
        {
            std::lock_guard lock(s.mutex);
            s.active.push_back(Transfer{client});
            start = !s.sending;
            s.sending = true;
        }
        if (start) {
            capy::run_async(s.ex)(send_loop(s));
        }
    }
}

static void run_io_context(corosio::io_context& ctx) {
    ctx.run();
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <file_path> [port] [threads]\n";
        return EXIT_FAILURE;
    }

    const fs::path file_path = argv[1];
    int port = DEFAULT_PORT;
    int threads = 1;

    if (argc >= 3) {
        port = std::stoi(argv[2]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port.\n";
            return EXIT_FAILURE;
        }
    }

    if (argc == 4) {
        threads = std::stoi(argv[3]);
        if (threads <= 0 || threads > MAX_THREADS) {
            std::cerr << "Invalid thread count.\n";
            return EXIT_FAILURE;
        }
    }

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        std::cerr << "Input path is not a regular file: " << file_path << "\n";
        return EXIT_FAILURE;
    }

    FileMapping mapping{};
    try {
        mapping = map_file_read_only(file_path);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    const std::span<const char> payload(mapping.data, mapping.size);
    const std::size_t dgram_size = dgram_bytes();

    try {
        corosio::io_context ctx;
        corosio::udp_socket sock(ctx);

        if (auto ec = sock.open(corosio::udp::v4())) {
            std::cerr << "open: " << ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        sock.set_option(corosio::socket_option::reuse_address(true));

        if (auto ec = sock.bind(corosio::endpoint(
                corosio::ipv4_address::any(), static_cast<std::uint16_t>(port)))) {
            std::cerr << "bind: " << ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        Server server{sock, ctx.get_executor(), {}, payload, dgram_size, {}};
        capy::run_async(server.ex)(serve_loop(server));

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back(run_io_context, std::ref(ctx));
        }

        for (auto& worker : pool) {
            worker.join();
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    unmap_file(mapping);
    return EXIT_SUCCESS;
}
