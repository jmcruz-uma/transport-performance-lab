/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * async-berkeley UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * One socket, no per-client state: on each request datagram the whole file is
 * streamed back to the requester's address as fixed-size datagrams
 * (DGRAM_BYTES, default the max IPv4 UDP payload, 65507 bytes), followed by a
 * zero-length datagram -- the end-of-transfer sentinel, since UDP has no
 * end-of-stream of its own.
 *
 * Deliberately uses the OS's default socket buffer sizes, like every other
 * scenario: any loss it causes at these datagram sizes is a real, comparable
 * measurement, not an artifact of tuning some implementations and not others.
 */

#include <io/io.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;
// 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
// payload a single sendmsg() can carry; anything above it fails with EMSGSIZE.
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
        ::close(mapping.fd);
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
        ::close(mapping.fd);
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

static constexpr auto error_handler = [](const auto& error) {
    if constexpr (std::is_same_v<std::decay_t<decltype(error)>, int>) {
        std::cerr << std::error_code(error, std::system_category()).message() << "\n";
    } else {
        std::cerr << "async operation failed\n";
    }
};

struct SendState {
    const dialog& server;   // one shared socket; every send targets `client`
    sockaddr_in client;
    std::span<const char> payload;
    std::size_t sent = 0;
    std::size_t dgram_size;
};

// Sends the next chunk to state->client; once the payload is exhausted, sends
// one zero-length datagram (the end-of-transfer sentinel) and stops.
static void send_next(async_scope& scope, std::shared_ptr<SendState> state) {
    auto msg = std::make_shared<message>();
    msg->address = socket_address<sockaddr_in>(&state->client);

    const std::size_t remaining = state->payload.size() - state->sent;
    const std::size_t n = std::min(state->dgram_size, remaining);

    msg->buffers.emplace_back(
        const_cast<char*>(state->payload.data() + state->sent),
        n
    );

    auto operation =
        sendmsg(state->server, *msg, 0)
        | then([&scope, state, msg, n](ssize_t bytes_sent) {
            if (n > 0) {
                if (bytes_sent <= 0) {
                    return;  // real chunk failed to send: give up on this client
                }
                state->sent += static_cast<std::size_t>(bytes_sent);
                send_next(scope, state);  // more data, or the n==0 sentinel next
            }
            // n == 0: the sentinel was just sent, this client is done.
        })
        | upon_error([state, msg](const auto& error) {
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

struct RequestBuffer {
    std::array<char, 64> data{};
};

static void serve_requests(
    async_scope& scope,
    const dialog& server,
    std::span<const char> payload,
    std::size_t dgram_size
) {
    auto req_buf = std::make_shared<RequestBuffer>();
    auto req_msg = std::make_shared<message>();
    // recvmsg() only captures the sender's address if msg_name already points
    // at writable storage (socket_message::operator message_header() copies
    // msg_name from `address` only `if (address)`); an empty optional gives
    // the kernel nowhere to write it.
    req_msg->address = make_address<sockaddr_in>();
    req_msg->buffers.emplace_back(req_buf->data.data(), req_buf->data.size());

    auto operation =
        recvmsg(server, *req_msg, 0)
        | then([&scope, &server, payload, dgram_size, req_buf, req_msg](ssize_t bytes_received) {
            if (bytes_received > 0 && req_msg->address) {
                const sockaddr_in client_addr = **req_msg->address;
                auto state = std::make_shared<SendState>(
                    SendState{server, client_addr, payload, 0, dgram_size});
                send_next(scope, state);
            }
            serve_requests(scope, server, payload, dgram_size);  // always re-arm
        })
        | upon_error([req_buf, req_msg](const auto& error) {
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

static void run_triggers(triggers& trigs) {
    while (trigs.wait()) {
    }
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
        async_scope scope;
        triggers trigs;

        auto server = trigs.emplace(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        auto server_address = make_address<sockaddr_in>();
        server_address->sin_family = AF_INET;
        server_address->sin_addr.s_addr = htonl(INADDR_ANY);
        server_address->sin_port = htons(static_cast<std::uint16_t>(port));

        socket_option<int> reuse{1};
        if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reuse)) {
            throw std::system_error(
                {errno, std::system_category()},
                "setsockopt failed"
            );
        }

        if (::io::bind(server, server_address)) {
            throw std::system_error(
                {errno, std::system_category()},
                "bind failed"
            );
        }

        serve_requests(scope, server, payload, dgram_size);

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back(run_triggers, std::ref(trigs));
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
