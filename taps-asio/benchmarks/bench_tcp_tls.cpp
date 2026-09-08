/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TAPS raw-byte file download benchmark over TLS 1.3 (scenario "tls").
 *
 * Identical to bench_tcp.cpp (streaming: receive() the byte stream as it
 * arrives, count bytes, stop at EOF) with ONE change: the Preconnection carries
 * taps::SecurityParameters, so a TLS 1.3 handshake -- pinned CA, SNI, ALPN --
 * runs during establishment. The client does NOT pin the cipher suite or group:
 * the server is the single authority for the negotiated parameters (see
 * ../../tls/tls_common.hpp and the design notes). TLS constants come from
 * tls_common.hpp.
 */

#include "taps/taps_api.h"

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
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "tls_common.hpp"

constexpr int DEFAULT_PORT = 8080;
constexpr std::uint64_t EXPECTED_FILE_SIZE_BYTES = 100ull * 1024ull * 1024ull;

static int g_port = DEFAULT_PORT;

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

static taps::SecurityParameters client_security() {
    taps::SecurityParameters security;
    security.require_tls();
    security.set_tls_version_range(taps::TLSVersion::TLS_1_3, taps::TLSVersion::TLS_1_3);
    security.add_trust_anchor(tlscommon::env_or("TLS_CA", "../tls/ca.crt"));
    security.set_server_name(tlscommon::kServerName);
    security.add_alpn(tlscommon::kAlpn);
    // Cipher suite / group deliberately not pinned here: forced by the server.
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

static asio::awaitable<std::uint64_t> receive_data(
    asio::io_context& io_context,
    const char* ip,
    int port
) {
    taps::TransportServices transport_services(io_context);

    auto connection = co_await connect_to_server(transport_services, ip, port);
    if (!connection) {
        co_return 0;
    }

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) {
        print_tls_identity(*connection, "client");
    }

    std::uint64_t total_bytes = 0;

    while (true) {
        auto receive_result = co_await connection->receive();

        if (!receive_result) {
            break;
        }

        auto message = std::move(*receive_result);
        const auto data = message.as_bytes();

        if (data.empty()) {
            break;
        }

        total_bytes += static_cast<std::uint64_t>(data.size());

        benchmark::DoNotOptimize(data.data());
        benchmark::DoNotOptimize(total_bytes);
        benchmark::ClobberMemory();
    }

    co_return total_bytes;
}

static bool run_benchmark_download(
    const char* ip,
    int port,
    std::uint64_t& downloaded_bytes
) {
    asio::io_context io_context;

    auto result = asio::co_spawn(
        io_context,
        receive_data(io_context, ip, port),
        asio::use_future
    );

    io_context.run();

    downloaded_bytes = result.get();
    return downloaded_bytes > 0;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;
        const bool ok = run_benchmark_download(ip, port, downloaded_bytes);

        if (!ok) {
            state.SkipWithError("Download failed.");
            break;
        }

        if (downloaded_bytes != EXPECTED_FILE_SIZE_BYTES) {
            state.SkipWithError("Downloaded byte count does not match expected file size.");
            break;
        }

        bytes_processed += downloaded_bytes;
        last_downloaded_bytes = downloaded_bytes;
    }

    state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));
    state.counters["downloaded_bytes"] = static_cast<double>(last_downloaded_bytes);
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

    benchmark::Initialize(&filtered_argc, argv);

    if (benchmark::ReportUnrecognizedArguments(filtered_argc, argv)) {
        return EXIT_FAILURE;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    return EXIT_SUCCESS;
}
