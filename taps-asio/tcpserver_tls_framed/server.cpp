/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with TAPS over TLS 1.3, length-prefixed framing (scenario
 * "tls_framed").
 *
 * Same shape as ../tcpserver_tls/server.cpp (SecurityParameters, one
 * connection.send() per message, TLS_IDENTITY) with framing added: the
 * connection carries a LengthPrefixedFramer and the server sends N discrete
 * Messages whose sizes come from the shared manifest (tls/manifest.txt, path in
 * TLS_MANIFEST). The message bodies are slices of the mmap'd payload, walked in
 * order and wrapped around, so the wire bytes stay deterministic and
 * measurement-neutral. After the last message the connection is closed, which
 * the client sees as the end-of-stream sentinel.
 */

#include "taps/taps_api.h"
#include "taps/message_framer.h"

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
#include <fstream>
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

// One message size in bytes per line; '#' comments and blank lines ignored.
static std::vector<std::size_t> read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    std::vector<std::size_t> sizes;
    std::string line;
    while (std::getline(in, line)) {
        std::size_t b = line.find_first_not_of(" \t\r\n");
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

// Format must stay byte-identical to tlscommon::print_tls_identity.
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

static asio::awaitable<void> send_messages(
    taps::Connection& connection,
    std::span<const char> payload,
    const std::vector<std::size_t>& sizes
) {
    std::size_t offset = 0;
    for (std::size_t size : sizes) {
        if (size > payload.size()) {
            size = payload.size();
        }
        if (offset + size > payload.size()) {
            offset = 0;
        }

        auto send_result = co_await connection.send(
            taps::make_message_view(std::string_view(payload.data() + offset, size))
        );
        if (!send_result) {
            std::cerr << "send failed: " << send_result.error().message() << "\n";
            co_return;
        }
        offset += size;
    }
    co_return;
}

static asio::awaitable<void> serve_client(
    std::unique_ptr<taps::Connection> connection,
    std::span<const char> payload,
    const std::vector<std::size_t>& sizes
) {
    static std::atomic<bool> printed{false};
    try {
        connection->set_framer(std::make_unique<taps::LengthPrefixedFramer>());
        if (!printed.exchange(true)) {
            print_tls_identity(*connection, "server");
        }
        co_await send_messages(*connection, payload, sizes);
        co_await connection->close();
    } catch (const std::exception& e) {
        std::cerr << "serve_client exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "serve_client unknown exception\n";
    }
    co_return;
}

static asio::awaitable<void> accept_loop(
    taps::Listener& listener,
    std::span<const char> payload,
    const std::vector<std::size_t>& sizes
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
            serve_client(std::move(*accept_result), payload, sizes),
            asio::detached
        );
    }
}

static asio::awaitable<void> listen_loop(
    asio::io_context& io_context,
    int port,
    std::span<const char> payload,
    const std::vector<std::size_t>& sizes
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

    std::cout << "TAPS TLS framed server listening on port " << port << "\n";
    std::cout << sizes.size() << " length-prefixed messages per client\n";

    co_await accept_loop(*listener, payload, sizes);
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

    try {
        asio::io_context io_context;
        auto work_guard = asio::make_work_guard(io_context);

        asio::co_spawn(
            io_context,
            listen_loop(io_context, port, payload, sizes),
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
