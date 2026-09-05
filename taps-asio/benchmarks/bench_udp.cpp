/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TAPS UDP file download benchmark (scenarios "udp_k64"/"udp_k1400", E4).
 * RELIABILITY::AVOID selects UDP (ActiveUDPConnection). Sends one request
 * Message, then reads Messages -- one per datagram -- until the server's
 * empty-Message end-of-transfer sentinel. UDP gives no delivery guarantee:
 * unlike the TCP scenarios, downloaded_bytes may legitimately fall short of
 * the file size under loss -- that is what this scenario measures, not a bug.
 */

#include "taps/taps_api.h"

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <utility>

using namespace asio::experimental::awaitable_operators;

constexpr int DEFAULT_PORT = 8080;

static int g_port = DEFAULT_PORT;

static asio::awaitable<std::uint64_t> receive_datagrams(taps::Connection& connection) {
    // Request datagram: content is irrelevant, only its arrival matters (it
    // gives PassiveUDPConnection this client's address to reply to). One byte,
    // not zero -- a zero-length send would be indistinguishable from the
    // sentinel this same loop watches for below.
    const std::array<std::uint8_t, 1> request{'r'};
    (void)co_await connection.send(taps::make_message_view(std::span<const std::uint8_t>(request)));

    std::uint64_t total_bytes = 0;

    while (true) {
        auto receive_result = co_await connection.receive();

        if (!receive_result) {
            break;
        }

        auto message = std::move(*receive_result);
        const std::size_t n = message.size();

        if (n == 0) {
            break;  // empty Message: end-of-transfer sentinel
        }

        total_bytes += static_cast<std::uint64_t>(n);

        const auto data = message.as_bytes();
        benchmark::DoNotOptimize(data.data());
        benchmark::DoNotOptimize(total_bytes);
        benchmark::ClobberMemory();
    }

    co_return total_bytes;
}

static asio::awaitable<std::uint64_t> run_download(
    asio::io_context& io_context, const char* ip, int port
) {
    taps::TransportServices transport_services(io_context);
    taps::TransportProperties properties;
    properties.set(taps::PropertyKey::RELIABILITY, taps::SelectionProperty::AVOID);

    auto preconnection = transport_services.preconnect(
        taps::LocalEndpoint{}, taps::RemoteEndpoint{ip, static_cast<std::uint16_t>(port)},
        std::move(properties));

    auto connection_result = co_await preconnection.initiate();
    if (!connection_result) {
        co_return 0;
    }
    auto connection = std::move(*connection_result);

    // Bound the exchange: unlike TCP, nothing signals a lost final sentinel
    // on its own. Racing means the timer is cancelled the moment the real
    // exchange finishes, so a fast transfer never waits out the timeout.
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::seconds(5));

    auto raced = co_await (
        receive_datagrams(*connection) ||
        timer.async_wait(asio::use_awaitable)
    );

    co_return raced.index() == 0 ? std::get<0>(raced) : 0;
}

static bool run_benchmark_download(const char* ip, int port, std::uint64_t& downloaded_bytes) {
    asio::io_context io_context;

    auto result = asio::co_spawn(
        io_context, run_download(io_context, ip, port), asio::use_future);

    io_context.run();

    downloaded_bytes = result.get();
    return downloaded_bytes > 0;
}

static void BM_UDP_FileDownload(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;

        if (!run_benchmark_download(ip, port, downloaded_bytes)) {
            state.SkipWithError("Download failed.");
            break;
        }

        bytes_processed += downloaded_bytes;
        last_downloaded_bytes = downloaded_bytes;
    }

    state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));
    state.counters["downloaded_bytes"] = static_cast<double>(last_downloaded_bytes);
}

BENCHMARK(BM_UDP_FileDownload)
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
