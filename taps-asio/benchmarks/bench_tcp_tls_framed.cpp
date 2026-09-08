/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TAPS file download benchmark over TLS 1.3 with length-prefixed framing
 * (scenario "tls_framed").
 *
 * Like bench_tcp_tls.cpp (SecurityParameters, receive() loop, TLS_IDENTITY) with
 * a LengthPrefixedFramer on the connection: each receive() yields one discrete
 * Message. The client reads the shared manifest (tls/manifest.txt, path in
 * TLS_MANIFEST) to know how many messages and how many bytes to expect, and
 * fails the run if the aggregate does not match. The cipher suite / group are
 * not pinned here; the server forces them.
 */

#include "taps/taps_api.h"
#include "taps/message_framer.h"

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "tls_common.hpp"

constexpr int DEFAULT_PORT = 8080;

static int g_port = DEFAULT_PORT;

struct ManifestExpectation {
    std::uint64_t count = 0;
    std::uint64_t total_bytes = 0;
};

// One message size in bytes per line; '#' comments and blank lines ignored.
static ManifestExpectation read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    ManifestExpectation e;
    std::string line;
    while (std::getline(in, line)) {
        std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') {
            continue;
        }
        e.total_bytes += std::stoull(line.substr(b));
        ++e.count;
    }
    if (e.count == 0) {
        throw std::runtime_error("manifest has no message sizes: " + path);
    }
    return e;
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

static taps::SecurityParameters client_security() {
    taps::SecurityParameters security;
    security.require_tls();
    security.set_tls_version_range(taps::TLSVersion::TLS_1_3, taps::TLSVersion::TLS_1_3);
    security.add_trust_anchor(tlscommon::env_or("TLS_CA", "../tls/ca.crt"));
    security.set_server_name(tlscommon::kServerName);
    security.add_alpn(tlscommon::kAlpn);
    return security;
}

static asio::awaitable<std::unique_ptr<taps::Connection>> connect_to_server(
    taps::TransportServices& transport_services,
    const char* ip,
    int port
) {
    taps::TransportProperties properties;
    properties.set(taps::PropertyKey::RELIABILITY, taps::SelectionProperty::REQUIRE);
    properties.set(taps::PropertyKey::PRESERVE_ORDER, taps::SelectionProperty::REQUIRE);

    auto preconnection = transport_services.preconnect(
        taps::LocalEndpoint{},
        taps::RemoteEndpoint{ip, static_cast<std::uint16_t>(port)},
        std::move(properties),
        client_security()
    );

    auto connection_result = co_await preconnection.initiate();
    if (!connection_result) {
        co_return nullptr;
    }
    co_return std::move(*connection_result);
}

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
};

static asio::awaitable<DownloadResult> receive_data(
    asio::io_context& io_context,
    const char* ip,
    int port
) {
    taps::TransportServices transport_services(io_context);

    auto connection = co_await connect_to_server(transport_services, ip, port);
    if (!connection) {
        co_return DownloadResult{};
    }

    connection->set_framer(std::make_unique<taps::LengthPrefixedFramer>());

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) {
        print_tls_identity(*connection, "client");
    }

    DownloadResult result;
    while (true) {
        auto receive_result = co_await connection->receive();
        if (!receive_result) {
            break;
        }

        auto message = std::move(*receive_result);
        const auto data = message.as_bytes();
        if (data.empty()) {
            break;  // end-of-stream sentinel
        }

        ++result.messages;
        result.bytes += static_cast<std::uint64_t>(data.size());

        benchmark::DoNotOptimize(data.data());
        benchmark::DoNotOptimize(result.bytes);
        benchmark::ClobberMemory();
    }

    co_return result;
}

static DownloadResult run_benchmark_download(const char* ip, int port) {
    asio::io_context io_context;

    auto future = asio::co_spawn(
        io_context,
        receive_data(io_context, ip, port),
        asio::use_future
    );

    io_context.run();
    return future.get();
}

static ManifestExpectation g_expected;

static void BM_TCP_FileDownload(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_messages = 0;

    for (auto _ : state) {
        (void)_;

        const DownloadResult r = run_benchmark_download(ip, port);

        if (r.bytes == 0) {
            state.SkipWithError("Download failed.");
            break;
        }
        if (r.messages != g_expected.count || r.bytes != g_expected.total_bytes) {
            state.SkipWithError("Received aggregate does not match the manifest.");
            break;
        }

        bytes_processed += r.bytes;
        last_messages = r.messages;
    }

    state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));
    state.counters["messages"] = static_cast<double>(last_messages);
}

BENCHMARK(BM_TCP_FileDownload)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

int main(int argc, char** argv) {
    const std::string prefix = "--server_port=";

    int filtered_argc = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) {
            g_port = std::stoi(arg.substr(prefix.size()));
        } else {
            argv[filtered_argc++] = argv[i];
        }
    }
    argv[filtered_argc] = nullptr;

    try {
        g_expected = read_manifest(tlscommon::env_or("TLS_MANIFEST", "../tls/manifest.txt"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    benchmark::Initialize(&filtered_argc, argv);
    if (benchmark::ReportUnrecognizedArguments(filtered_argc, argv)) {
        return EXIT_FAILURE;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return EXIT_SUCCESS;
}
