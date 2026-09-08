/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with TAPS over TLS 1.3 (scenario "tls").
 *
 * Same raw-byte, mmap'd-file, thread-pool shape as ../tcpserver/server.cpp with
 * two changes:
 *   1. The Listener is created with taps::SecurityParameters, so every accepted
 *      Connection runs a TLS 1.3 handshake before any data flows. The server is
 *      the single authority for the negotiated parameters: it offers exactly one
 *      cipher suite, one group and one ALPN id (from ../../tls/tls_common.hpp),
 *      so the peer's negotiated values are forced regardless of what each client
 *      arm is able to pin.
 *   2. The whole payload goes out in ONE connection.send(); there is no
 *      tcp_wmem-sized chunk loop (that was a comparability artefact — OpenSSL
 *      fragments the plaintext into <=16 KiB records internally, identically for
 *      every arm).
 */

#include "taps/taps_api.h"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tls_common.hpp"

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;

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
        mapping.data = nullptr;
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

// TLS parameters the peer negotiated, printed once per process. Format must stay
// byte-identical to tlscommon::print_tls_identity so the harness comparability
// check (run_bench.py) can assert every arm matches.
static void print_tls_identity(taps::Connection& connection, const char* who) {
    const auto info = connection.security_info();
    if (!info) {
        std::fprintf(stderr, "TLS_IDENTITY who=%s ERROR no security info\n", who);
        return;
    }
    std::fprintf(stderr,
                 "TLS_IDENTITY who=%s openssl=\"%s\" version=%s cipher=%s alpn=%s\n",
                 who, info->openssl_version.c_str(), info->tls_version.c_str(),
                 info->cipher.c_str(), info->alpn.c_str());
}

static taps::SecurityParameters server_security() {
    taps::SecurityParameters security;
    security.require_tls();
    security.set_tls_version_range(taps::TLSVersion::TLS_1_3, taps::TLSVersion::TLS_1_3);
    security.set_certificate_chain_file(tlscommon::env_or("TLS_CERT", "../tls/server.crt"));
    security.set_private_key_file(tlscommon::env_or("TLS_KEY", "../tls/server.key"));
    security.add_alpn(tlscommon::kAlpn);
    security.set_ciphersuites({tlscommon::kCipherSuite});
    security.set_supported_groups({tlscommon::kGroup});
    return security;
}

static asio::awaitable<void> send_file(
    taps::Connection& connection,
    std::span<const char> payload
) {
    // One send() of the entire payload: no application-level chunking.
    auto send_result = co_await connection.send(
        taps::make_message_view(std::string_view(payload.data(), payload.size()))
    );

    if (!send_result) {
        std::cerr << "send failed: " << send_result.error().message() << "\n";
    }

    co_return;
}

static asio::awaitable<void> serve_client(
    std::unique_ptr<taps::Connection> connection,
    std::span<const char> payload
) {
    static std::atomic<bool> printed{false};
    try {
        if (!printed.exchange(true)) {
            print_tls_identity(*connection, "server");
        }
        co_await send_file(*connection, payload);
    } catch (const std::exception& e) {
        std::cerr << "serve_client exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "serve_client unknown exception\n";
    }

    co_return;
}

static asio::awaitable<void> accept_loop(
    taps::Listener& listener,
    std::span<const char> payload
) {
    auto executor = co_await asio::this_coro::executor;

    while (true) {
        auto accept_result = co_await listener.accept();

        if (!accept_result) {
            std::cerr << "accept failed: " << accept_result.error().message() << "\n";
            continue;
        }

        asio::co_spawn(
            executor,
            serve_client(std::move(*accept_result), payload),
            asio::detached
        );
    }
}

static asio::awaitable<void> listen_loop(
    asio::io_context& io_context,
    int port,
    std::span<const char> payload
) {
    taps::TransportServices transport_services(io_context);

    taps::TransportProperties properties;
    properties.set(taps::PropertyKey::RELIABILITY, taps::SelectionProperty::REQUIRE);
    properties.set(taps::PropertyKey::PRESERVE_ORDER, taps::SelectionProperty::REQUIRE);

    auto listen_result = co_await transport_services.listen(
        taps::LocalEndpoint{"0.0.0.0", static_cast<std::uint16_t>(port)},
        std::move(properties),
        server_security()
    );

    if (!listen_result) {
        std::cerr << "listen failed: " << listen_result.error().message() << "\n";
        co_return;
    }

    auto listener = std::move(*listen_result);

    std::cout << "TAPS TLS server listening on port " << port << "\n";
    std::cout << "single send() of " << payload.size() << " bytes per client\n";

    co_await accept_loop(*listener, payload);
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

    try {
        asio::io_context io_context;
        auto work_guard = asio::make_work_guard(io_context);

        asio::co_spawn(
            io_context,
            listen_loop(io_context, port, payload),
            asio::detached
        );

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back([&io_context]() {
                io_context.run();
            });
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
