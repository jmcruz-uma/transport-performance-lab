/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Asio, length-prefixed framing, NO security layer
 * (scenario "framed" -- the plaintext mirror of "tls_framed", replacing the
 * old "blocks" scenario).
 *
 * "blocks" tried to measure segment-by-segment Message consumption, but its
 * framer (taps::PassthroughFramer) can only ever emit after the connection's
 * half-close -- there is no boundary marker anywhere on a raw-until-close
 * wire for it to key off, so it was structurally incapable of the
 * incremental delivery its own description implied. taps::LengthPrefixedFramer
 * already does real incremental delivery (its parse() ignores at_eof entirely
 * and emits the instant a full record has arrived), and "tls_framed" already
 * proves it works end to end on real hardware. This scenario is that same
 * proven design -- same manifest, same frame format, same deframing loop --
 * with the TLS record layer removed, so framing cost can be measured on its
 * own instead of only ever bundled with TLS's cost.
 *
 * Same shape as ../tcpserver/server.cpp (mmap'd payload, thread pool) with
 * framing added: instead of one write loop over the whole payload, the server
 * sends N discrete messages whose sizes come from the shared manifest
 * (tls/manifest.txt, path in MANIFEST -- yes, still under tls/: it's the same
 * workload file "tls_framed" uses, reused here so the two scenarios are
 * directly comparable message-for-message; nothing about the manifest itself
 * is TLS-specific). Each message goes out as a single gather write of
 * {4-byte big-endian length, body}; the body bytes are slices of the mmap'd
 * payload, walked in order and wrapped around, so the wire stays
 * deterministic. This is byte-for-byte the frame format taps_cpp's
 * LengthPrefixedFramer produces, so the TAPS arm and this one exchange
 * identical wire data.
 */

#include <csignal>

#include <asio.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr int MAX_THREADS = 256;
constexpr std::size_t LENGTH_PREFIX = 4;  // 4-byte big-endian, matches taps LengthPrefixedFramer

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : fallback;
}

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

// One message size in bytes per line; '#' comments and blank lines ignored.
static std::vector<std::size_t> read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    std::vector<std::size_t> sizes;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') {
            continue;
        }
        sizes.push_back(static_cast<std::size_t>(std::stoull(line.substr(b))));
    }
    if (sizes.empty()) {
        throw std::runtime_error("manifest has no message sizes: " + path);
    }
    return sizes;
}

static asio::awaitable<void> send_messages(tcp::socket& socket, std::span<const char> payload,
                                           const std::vector<std::size_t>& sizes) {
    std::size_t offset = 0;
    for (std::size_t size : sizes) {
        if (size > payload.size()) {
            size = payload.size();
        }
        if (offset + size > payload.size()) {
            offset = 0;
        }

        std::array<unsigned char, LENGTH_PREFIX> header{};
        const auto len = static_cast<std::uint32_t>(size);
        header[0] = static_cast<unsigned char>(len >> 24);
        header[1] = static_cast<unsigned char>(len >> 16);
        header[2] = static_cast<unsigned char>(len >> 8);
        header[3] = static_cast<unsigned char>(len);

        const std::array<asio::const_buffer, 2> frame{
            asio::buffer(header),
            asio::buffer(payload.data() + offset, size)};
        co_await asio::async_write(socket, frame, asio::use_awaitable);
        offset += size;
    }
    co_return;
}

static asio::awaitable<void> serve_client(tcp::socket socket, std::span<const char> payload,
                                          const std::vector<std::size_t>& sizes) {
    try {
        co_await send_messages(socket, payload, sizes);
        std::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (...) {
    }
    co_return;
}

static asio::awaitable<void> accept_loop(tcp::acceptor& acceptor,
                                         std::span<const char> payload,
                                         const std::vector<std::size_t>& sizes) {
    auto executor = co_await asio::this_coro::executor;
    while (true) {
        try {
            tcp::socket socket(executor);
            co_await acceptor.async_accept(socket, asio::use_awaitable);
            asio::co_spawn(executor, serve_client(std::move(socket), payload, sizes),
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

    std::vector<std::size_t> sizes;
    try {
        sizes = read_manifest(env_or("MANIFEST", "../tls/manifest.txt"));
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
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

        asio::co_spawn(io_context, accept_loop(acceptor, payload, sizes), asio::detached);

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
