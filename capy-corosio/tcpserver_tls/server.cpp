/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Corosio over TLS 1.3.
 *
 * Identical to ../tcpserver/server.cpp (raw-byte, mmap'd file, one write loop per
 * connection, thread pool) with ONE change: each accepted socket is wrapped in a
 * corosio::openssl_stream and a server handshake runs before the file is sent.
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
#include <boost/corosio.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tls_context.hpp>

#include "tls_common.hpp"

namespace fs = std::filesystem;
namespace corosio = boost::corosio;
namespace capy = boost::capy;

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

static capy::task<void> serve_client(corosio::tcp_socket sock, std::span<const char> file_view,
                                     const corosio::tls_context& ctx) {
    corosio::openssl_stream tls(std::move(sock), ctx);

    auto [hs_ec] = co_await tls.handshake(corosio::tls_role::server);
    if (hs_ec) co_return;

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) print_identity(tls.alpn_protocol(), "server");

    std::size_t sent = 0;
    while (sent < file_view.size()) {
        auto [ec, n] = co_await tls.write_some(
            capy::const_buffer(file_view.data() + sent, file_view.size() - sent));
        if (ec || n == 0) break;
        sent += static_cast<std::size_t>(n);
    }

    auto [sd_ec] = co_await tls.shutdown();
    (void)sd_ec;
    co_return;
}

static capy::task<void> accept_loop(corosio::io_context& ctx, corosio::tcp_acceptor& acceptor,
                                    std::span<const char> file_view,
                                    const corosio::tls_context& tls_ctx) {
    while (true) {
        corosio::tcp_socket sock(ctx);
        auto [ec] = co_await acceptor.accept(sock);
        if (ec) continue;
        capy::run_async(ctx.get_executor())(
            serve_client(std::move(sock), file_view, tls_ctx));
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
    const std::span<const char> file_view(mapping.data, mapping.size);

    corosio::tls_context tls_ctx;
    if (!configure_server_context(tls_ctx)) {
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    try {
        corosio::io_context ctx;
        corosio::tcp_acceptor acceptor(ctx);
        acceptor.open(corosio::tcp::v4());

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
            accept_loop(ctx, acceptor, file_view, tls_ctx));

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
