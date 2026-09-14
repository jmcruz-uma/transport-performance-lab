/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TAPS, BLOCKS model (scenario "blocks", E3).
 * One logical Message (PassthroughFramer, RFC 9623 5.2/6.1 -- the whole
 * connection is one Message), consumed segment by segment via blocks()
 * WITHOUT ever materialising it contiguously: gather=false means as_bytes()
 * is never called, so no gather copy happens anywhere on this path (each
 * block corresponds to what would be one Framer::parse() call in a real
 * protocol).
 */

#include "taps/taps_api.h"

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <utility>

constexpr int DEFAULT_PORT = 8080;
constexpr std::uint64_t EXPECTED_FILE_SIZE_BYTES = 100ull * 1024ull * 1024ull;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

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
        std::move(properties)
    );

    auto connection_result = co_await preconnection.initiate();

    if (!connection_result) {
        co_return nullptr;
    }

    co_return std::move(*connection_result);
}

static asio::awaitable<std::uint64_t> receive_blocks(
    asio::io_context& io_context,
    const char* ip,
    int port
) {
    taps::TransportServices transport_services(io_context);

    auto connection = co_await connect_to_server(transport_services, ip, port);
    if (!connection) {
        co_return 0;
    }

    // gather=false: the whole-connection Message is delivered as a chain of
    // pooled blocks, never gathered into one contiguous buffer.
    connection->set_framer(std::make_unique<taps::PassthroughFramer>(/*gather=*/false));

    std::uint64_t total_bytes = 0;

    while (true) {
        auto receive_result = co_await connection->receive();

        if (!receive_result) {
            break;
        }

        auto message = std::move(*receive_result);
        const auto segments = message.blocks();

        if (segments.empty()) {
            break;
        }

        for (const auto& segment : segments) {
            total_bytes += static_cast<std::uint64_t>(segment.size());

            benchmark::DoNotOptimize(segment.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();
        }
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
        receive_blocks(io_context, ip, port),
        asio::use_future
    );

    io_context.run();

    downloaded_bytes = result.get();
    return downloaded_bytes > 0;
}

static void BM_TCP_Blocks(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
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

BENCHMARK(BM_TCP_Blocks)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

int main(int argc, char** argv) {
    const std::string port_prefix = "--server_port=";
    const std::string ip_prefix = "--server_ip=";

    int filtered_argc = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg.rfind(port_prefix, 0) == 0) {
            g_port = std::stoi(arg.substr(port_prefix.size()));
        } else if (arg.rfind(ip_prefix, 0) == 0) {
            g_server_ip = arg.substr(ip_prefix.size());
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
