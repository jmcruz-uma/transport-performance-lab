/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Corosio, length-prefixed framing, NO security layer
 * (scenario "framed" -- the plaintext mirror of "tls_framed", replacing the
 * old "blocks" scenario -- see ../../asio/tcpserver_framed/server.cpp for why).
 *
 * Same mmap'd-file setup as ../tcpserver/server.cpp, with framing: sends N
 * discrete messages whose sizes come from the shared manifest
 * (tls/manifest.txt, path in MANIFEST -- still under tls/: it's the same
 * workload file "tls_framed" uses, reused here so the two scenarios are
 * directly comparable message-for-message), each as a 4-byte big-endian
 * length followed by the body. capy::write() (boost/capy/write.hpp) is a
 * composed gather-write over a ConstBufferSequence, and works directly on the
 * raw corosio::tcp_socket the same way it works on the TLS arm's
 * openssl_stream -- so each message is still ONE co_await
 * capy::write(sock, {header, body}), matching the asio and taps-asio framed
 * servers exactly. Wire bytes are byte-for-byte what taps_cpp's
 * LengthPrefixedFramer produces.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio.hpp>

namespace fs = std::filesystem;
namespace corosio = boost::corosio;
namespace capy = boost::capy;

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
    if (mapping.fd == -1) throw std::runtime_error("Failed to open file: " + path.string());
    if (mapping.size == 0) return mapping;
    void* ptr = mmap(nullptr, mapping.size, PROT_READ, MAP_PRIVATE, mapping.fd, 0);
    if (ptr == MAP_FAILED) { close(mapping.fd); throw std::runtime_error("mmap failed."); }
    mapping.data = static_cast<const char*>(ptr);
    return mapping;
}

static void unmap_file(FileMapping& mapping) {
    if (mapping.data != nullptr && mapping.size > 0)
        munmap(const_cast<char*>(mapping.data), mapping.size);
    if (mapping.fd != -1) close(mapping.fd);
    mapping = FileMapping{};
}

// One message size in bytes per line; '#' comments and blank lines ignored.
static std::vector<std::size_t> read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open manifest: " + path);
    std::vector<std::size_t> sizes;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        sizes.push_back(static_cast<std::size_t>(std::stoull(line.substr(b))));
    }
    if (sizes.empty()) throw std::runtime_error("manifest has no message sizes: " + path);
    return sizes;
}

static capy::task<void> send_messages(corosio::tcp_socket& sock,
                                      std::span<const char> payload,
                                      const std::vector<std::size_t>& sizes) {
    std::size_t offset = 0;
    for (std::size_t size : sizes) {
        if (size > payload.size()) size = payload.size();
        if (offset + size > payload.size()) offset = 0;

        const auto len = static_cast<std::uint32_t>(size);
        const unsigned char header[LENGTH_PREFIX] = {
            static_cast<unsigned char>(len >> 24), static_cast<unsigned char>(len >> 16),
            static_cast<unsigned char>(len >> 8), static_cast<unsigned char>(len)};

        // One gather-write of {header, body}, same shape as asio's
        // async_write(stream, {hdr, body}) and taps_cpp's internal send().
        const std::array<capy::const_buffer, 2> frame = {
            capy::const_buffer(header, LENGTH_PREFIX),
            capy::const_buffer(payload.data() + offset, size)};

        auto [ec, n] = co_await capy::write(sock, frame);
        if (ec || n != LENGTH_PREFIX + size) co_return;

        offset += size;
    }
    co_return;
}

static capy::task<void> serve_client(corosio::tcp_socket sock, std::span<const char> payload,
                                     const std::vector<std::size_t>& sizes) {
    co_await send_messages(sock, payload, sizes);
    co_return;
}

static capy::task<void> accept_loop(corosio::io_context& ctx, corosio::tcp_acceptor& acceptor,
                                    std::span<const char> payload,
                                    const std::vector<std::size_t>& sizes) {
    while (true) {
        corosio::tcp_socket sock(ctx);
        auto [ec] = co_await acceptor.accept(sock);
        if (ec) continue;
        capy::run_async(ctx.get_executor())(serve_client(std::move(sock), payload, sizes));
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
        corosio::io_context ctx;
        corosio::tcp_acceptor acceptor(ctx);
        if (auto open_ec = acceptor.open(corosio::tcp::v4())) {
            std::cerr << "open: " << open_ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }
        acceptor.set_option(corosio::socket_option::reuse_address(true));

        if (auto bind_ec = acceptor.bind(corosio::endpoint(static_cast<std::uint16_t>(port)))) {
            std::cerr << "bind: " << bind_ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }
        if (auto listen_ec = acceptor.listen(BACKLOG)) {
            std::cerr << "listen: " << listen_ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        capy::run_async(ctx.get_executor())(accept_loop(ctx, acceptor, payload, sizes));

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        for (int i = 0; i < threads; ++i)
            pool.emplace_back([&ctx] { ctx.run(); });
        for (auto& worker : pool) worker.join();

        unmap_file(mapping);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }
}
