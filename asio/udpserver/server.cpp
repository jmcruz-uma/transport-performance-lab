/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * One socket, no per-client state: on each request datagram the whole file is
 * streamed back to the requester's address as fixed-size datagrams
 * (DGRAM_BYTES, default the max IPv4 UDP payload, 65507 bytes), followed by a
 * zero-length datagram -- the end-of-transfer sentinel, since UDP has no
 * end-of-stream of its own.
 *
 * All socket operations run on one strand: asio only allows one outstanding
 * async op per direction per socket, and several clients' response streams
 * would otherwise issue concurrent async_send_to() calls on this shared
 * socket. A strand serialises them without needing a socket (or a thread) per
 * client.
 *
 * Deliberately uses the OS's default socket buffer sizes, like every other
 * scenario: any loss it causes at these datagram sizes is a real, comparable
 * measurement, not an artifact of tuning some implementations and not others.
 */

#include <csignal>

#include <asio.hpp>

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
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using udp = asio::ip::udp;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;
// 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
// payload a single send can carry; anything above it fails.
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

static asio::awaitable<void> send_to_client(
    udp::socket& socket, udp::endpoint client,
    std::span<const char> payload, std::size_t dgram_size
) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const std::size_t n = std::min(dgram_size, payload.size() - sent);
        std::error_code ec;
        co_await socket.async_send_to(
            asio::buffer(payload.data() + sent, n), client,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return;
        }
        sent += n;
    }
    // Zero-length datagram: end-of-transfer sentinel.
    std::error_code ec;
    co_await socket.async_send_to(
        asio::const_buffer(), client,
        asio::redirect_error(asio::use_awaitable, ec));
    co_return;
}

static asio::awaitable<void> serve_loop(
    udp::socket& socket, std::span<const char> payload, std::size_t dgram_size
) {
    auto executor = co_await asio::this_coro::executor;
    std::array<char, 64> request{};

    for (;;) {
        udp::endpoint client;
        std::error_code ec;
        co_await socket.async_receive_from(
            asio::buffer(request), client,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            continue;
        }
        asio::co_spawn(
            executor,
            send_to_client(socket, client, payload, dgram_size),
            asio::detached);
    }
}

static void run_io_context(asio::io_context& io_context) {
    io_context.run();
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

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
        asio::io_context io_context;
        auto work_guard = asio::make_work_guard(io_context);

        udp::socket socket(asio::make_strand(io_context));
        std::error_code ec;

        socket.open(udp::v4(), ec);
        if (ec) {
            std::cerr << "open: " << ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        socket.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "set_option: " << ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        socket.bind(udp::endpoint(udp::v4(), static_cast<unsigned short>(port)), ec);
        if (ec) {
            std::cerr << "bind: " << ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        asio::co_spawn(
            socket.get_executor(),
            serve_loop(socket, payload, dgram_size),
            asio::detached);

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back(run_io_context, std::ref(io_context));
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
