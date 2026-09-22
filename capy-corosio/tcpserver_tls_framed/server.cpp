/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Corosio over TLS 1.3, length-prefixed framing (scenario
 * "tls_framed").
 *
 * Same handshake and mmap'd-file setup as ../tcpserver_tls/server.cpp, with
 * framing: sends N discrete messages whose sizes come from the shared manifest
 * (tls/manifest.txt, path in TLS_MANIFEST), each as a 4-byte big-endian length
 * followed by the body. capy::write() (boost/capy/write.hpp) is a composed
 * gather-write over a ConstBufferSequence -- the same shape as
 * asio::async_write(stream, {hdr, body}) -- so each message is ONE
 * co_await capy::write(tls, {header, body}), matching the asio and taps-asio
 * servers exactly; unlike bsd-sockets, which is stuck with two SSL_write calls
 * because raw OpenSSL has no gather write. Wire bytes are byte-for-byte what
 * taps_cpp's LengthPrefixedFramer produces.
 *
 * Comparability note: corosio's portable tls_context pins the protocol version
 * (TLS 1.3), the cipher suite and ALPN, but exposes no named-group knob and no
 * accessor for the negotiated parameters. The key-exchange group is X25519 de
 * facto (OpenSSL 3.x default for TLS 1.3, the same backend every arm uses);
 * confirmed once out-of-band with a packet capture. Values come from
 * ../../tls/tls_common.hpp.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tls_context.hpp>

#include "tls_common.hpp"

#include <array>

namespace fs = std::filesystem;
namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr int MAX_THREADS = 256;
constexpr std::size_t LENGTH_PREFIX = 4;  // 4-byte big-endian, matches taps LengthPrefixedFramer

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

static void print_identity(std::string_view alpn, const char* who) {
    // Version and cipher are pinned in the context; a mismatch would fail the
    // handshake, so they are reported as configured.
    std::fprintf(stderr,
                 "TLS_IDENTITY who=%s openssl=\"%s\" version=TLSv1.3 cipher=%s alpn=%.*s\n",
                 who, OpenSSL_version(OPENSSL_VERSION), tlscommon::kCipherSuite,
                 static_cast<int>(alpn.size()), alpn.data());
}

static bool configure_server_context(corosio::tls_context& ctx) {
    if (ctx.use_certificate_chain_file(tlscommon::env_or("TLS_CERT", "../tls/server.crt")) ||
        ctx.use_private_key_file(tlscommon::env_or("TLS_KEY", "../tls/server.key"),
                                 corosio::tls_file_format::pem) ||
        ctx.set_min_protocol_version(corosio::tls_version::tls_1_3) ||
        ctx.set_max_protocol_version(corosio::tls_version::tls_1_3) ||
        ctx.set_ciphersuites_tls13(tlscommon::kCipherSuite) ||
        ctx.set_alpn({tlscommon::kAlpn})) {
        std::fprintf(stderr, "tls: server context configuration failed\n");
        return false;
    }
    return true;
}

static capy::task<void> send_messages(corosio::openssl_stream& tls,
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

        auto [ec, n] = co_await capy::write(tls, frame);
        if (ec || n != LENGTH_PREFIX + size) co_return;

        offset += size;
    }
    co_return;
}

static capy::task<void> serve_client(corosio::tcp_socket sock, std::span<const char> payload,
                                     const corosio::tls_context& ctx,
                                     const std::vector<std::size_t>& sizes) {
    corosio::openssl_stream tls(std::move(sock), ctx);

    auto [hs_ec] = co_await tls.handshake(corosio::tls_role::server);
    if (hs_ec) co_return;

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) print_identity(tls.alpn_protocol(), "server");

    co_await send_messages(tls, payload, sizes);

    auto [sd_ec] = co_await tls.shutdown();
    (void)sd_ec;
    co_return;
}

static capy::task<void> accept_loop(corosio::io_context& ctx, corosio::tcp_acceptor& acceptor,
                                    std::span<const char> payload,
                                    const corosio::tls_context& tls_ctx,
                                    const std::vector<std::size_t>& sizes) {
    while (true) {
        corosio::tcp_socket sock(ctx);
        auto [ec] = co_await acceptor.accept(sock);
        if (ec) continue;
        capy::run_async(ctx.get_executor())(
            serve_client(std::move(sock), payload, tls_ctx, sizes));
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
        sizes = read_manifest(tlscommon::env_or("TLS_MANIFEST", "../tls/manifest.txt"));
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

    corosio::tls_context tls_ctx;
    if (!configure_server_context(tls_ctx)) {
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    try {
        corosio::io_context ctx;
        corosio::tcp_acceptor acceptor(ctx);
        acceptor.open(corosio::tcp::v4());
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

        capy::run_async(ctx.get_executor())(
            accept_loop(ctx, acceptor, payload, tls_ctx, sizes));

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
