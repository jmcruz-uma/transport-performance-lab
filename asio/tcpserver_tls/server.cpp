/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Asio over TLS 1.3.
 *
 * Identical to ../tcpserver/server.cpp (raw-byte, mmap'd file, one write loop per
 * connection, thread pool) with ONE change: each accepted socket is wrapped in an
 * asio::ssl::stream and a server handshake runs before the file is sent. The TLS
 * parameters (cipher, group, ALPN, TLS 1.3 only) come from ../../tls/tls_common.hpp
 * so every arm is configured identically.
 */

#include <csignal>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "tls_common.hpp"

namespace fs = std::filesystem;
using tcp = asio::ip::tcp;
using tls_stream = asio::ssl::stream<tcp::socket>;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr int MAX_THREADS = 256;

struct FileMapping {
    int fd = -1;
    const char* data = nullptr;
    std::size_t size = 0;
};

static FileMapping map_file_read_only(const fs::path& path) {
    FileMapping mapping{};
    mapping.size = static_cast<std::size_t>(fs::file_size(path));
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
    mapping = FileMapping{};
}

static asio::awaitable<void> send_file(tls_stream& stream, std::span<const char> payload) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const std::size_t n = co_await stream.async_write_some(
            asio::buffer(payload.data() + sent, payload.size() - sent),
            asio::use_awaitable);
        if (n == 0) break;
        sent += n;
    }
    co_return;
}

static asio::awaitable<void> serve_client(tls_stream stream, std::span<const char> payload) {
    static std::atomic<bool> printed{false};
    try {
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        if (!printed.exchange(true)) {
            tlscommon::print_tls_identity(stream.native_handle(), "server");
        }
        co_await send_file(stream, payload);
        asio::error_code ec;
        co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
    } catch (...) {
    }
    co_return;
}

static asio::awaitable<void> accept_loop(tcp::acceptor& acceptor, asio::ssl::context& ssl_ctx,
                                         std::span<const char> payload) {
    auto executor = co_await asio::this_coro::executor;
    while (true) {
        try {
            tcp::socket socket(executor);
            co_await acceptor.async_accept(socket, asio::use_awaitable);
            asio::co_spawn(executor,
                           serve_client(tls_stream(std::move(socket), ssl_ctx), payload),
                           asio::detached);
        } catch (...) {
        }
    }
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
        if (port <= 0 || port > 65535) { std::cerr << "Invalid port.\n"; return EXIT_FAILURE; }
    }
    if (argc == 4) {
        threads = std::stoi(argv[3]);
        if (threads <= 0 || threads > MAX_THREADS) { std::cerr << "Invalid thread count.\n"; return EXIT_FAILURE; }
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

    asio::ssl::context ssl_ctx(asio::ssl::context::tls_server);
    if (!tlscommon::configure_server_ctx(
            ssl_ctx.native_handle(),
            tlscommon::env_or("TLS_CERT", "../tls/server.crt"),
            tlscommon::env_or("TLS_KEY", "../tls/server.key"))) {
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    try {
        asio::io_context io_context;
        auto work_guard = asio::make_work_guard(io_context);

        tcp::acceptor acceptor(io_context);
        std::error_code ec;
        acceptor.open(tcp::v4(), ec);
        if (ec) { std::cerr << "open: " << ec.message() << "\n"; unmap_file(mapping); return EXIT_FAILURE; }
        acceptor.set_option(asio::socket_base::reuse_address(true), ec);
        if (ec) { std::cerr << "set_option: " << ec.message() << "\n"; unmap_file(mapping); return EXIT_FAILURE; }
        acceptor.bind(tcp::endpoint(tcp::v4(), static_cast<unsigned short>(port)), ec);
        if (ec) { std::cerr << "bind: " << ec.message() << "\n"; unmap_file(mapping); return EXIT_FAILURE; }
        acceptor.listen(BACKLOG, ec);
        if (ec) { std::cerr << "listen: " << ec.message() << "\n"; unmap_file(mapping); return EXIT_FAILURE; }

        asio::co_spawn(io_context, accept_loop(acceptor, ssl_ctx, payload), asio::detached);

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        for (int i = 0; i < threads; ++i) {
            pool.emplace_back([&io_context] { io_context.run(); });
        }
        for (auto& worker : pool) worker.join();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    unmap_file(mapping);
    return EXIT_SUCCESS;
}
