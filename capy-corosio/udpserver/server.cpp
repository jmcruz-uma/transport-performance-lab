/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * One socket, no per-client state: on each request datagram the whole file is
 * streamed back to the requester's address as fixed-size datagrams
 * (DGRAM_BYTES, default the max IPv4 UDP payload, 65507 bytes), followed by a
 * zero-length datagram -- the end-of-transfer sentinel, since UDP has no
 * end-of-stream of its own.
 *
 * Requests are served one at a time (each response awaited fully before the
 * next recv_from()): Corosio's UDP socket, like Asio's, only documents one
 * outstanding op per direction, and without a confirmed strand-equivalent
 * this is the option that is unconditionally safe rather than merely assumed
 * safe under concurrent send_to() calls.
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

static capy::task<void> send_to_client(
    corosio::udp_socket& sock, corosio::endpoint client,
    std::span<const char> payload, std::size_t dgram_size
) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const std::size_t n = std::min(dgram_size, payload.size() - sent);
        auto [ec, sn] = co_await sock.send_to(
            capy::const_buffer(payload.data() + sent, n), client);
        if (ec || sn == 0) {
            co_return;
        }
        sent += static_cast<std::size_t>(sn);
    }
    // Zero-length datagram: end-of-transfer sentinel.
    co_await sock.send_to(capy::const_buffer(payload.data(), 0), client);
    co_return;
}

static capy::task<void> serve_loop(
    corosio::udp_socket& sock, std::span<const char> payload, std::size_t dgram_size
) {
    std::array<char, 64> request{};
    for (;;) {
        corosio::endpoint client;
        auto [ec, n] = co_await sock.recv_from(
            capy::mutable_buffer(request.data(), request.size()), client);
        if (ec) {
            continue;
        }
        co_await send_to_client(sock, client, payload, dgram_size);
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

        capy::run_async(ctx.get_executor())(
            serve_loop(sock, payload, dgram_size)
        );

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
