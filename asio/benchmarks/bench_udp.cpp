/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio UDP file download benchmark (scenarios "udp_k64"/"udp_k1400", E4).
 * Sends one request datagram, then reads datagrams until the server's
 * zero-length end-of-transfer sentinel. UDP gives no delivery guarantee:
 * unlike the TCP scenarios, downloaded_bytes may legitimately fall short of
 * the file size under loss -- that is what this scenario measures, not a bug.
 * OS default socket buffer sizes throughout, like every other scenario.
 */

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <string>
#include <system_error>

using namespace asio::experimental::awaitable_operators;
using udp = asio::ip::udp;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t RECV_BUFFER_BYTES = 65536;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

static asio::awaitable<bool> receive_datagrams(
    udp::socket& socket, udp::endpoint server, std::uint64_t& total_bytes
) {
    total_bytes = 0;

    std::error_code ec;
    co_await socket.async_send_to(
        asio::const_buffer("r", 1), server,
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        co_return false;
    }

    std::array<char, RECV_BUFFER_BYTES> buffer{};

    while (true) {
        udp::endpoint from;
        const std::size_t n = co_await socket.async_receive_from(
            asio::buffer(buffer), from,
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            break;  // timeout / error: stop with whatever arrived so far
        }

        if (n == 0) {
            break;  // zero-length datagram: end-of-transfer sentinel
        }

        total_bytes += static_cast<std::uint64_t>(n);

        benchmark::DoNotOptimize(buffer.data());
        benchmark::DoNotOptimize(total_bytes);
        benchmark::ClobberMemory();
    }

    co_return total_bytes > 0;
}

static asio::awaitable<bool> run_benchmark_client(
    const char* ip, int port, std::uint64_t& total_bytes
) {
    auto executor = co_await asio::this_coro::executor;
    udp::socket socket(executor);

    std::error_code ec;
    socket.open(udp::v4(), ec);
    if (ec) {
        co_return false;
    }

    udp::endpoint server(asio::ip::make_address(ip, ec), static_cast<unsigned short>(port));
    if (ec) {
        co_return false;
    }

    // Race the exchange against a timer: unlike TCP, nothing signals a lost
    // final sentinel on its own, so this bounds the wait instead of hanging
    // forever. Racing inside the coroutine (rather than stopping io_context
    // from outside) means the loser is cancelled and run() returns as soon as
    // either side finishes -- no separate watchdog bookkeeping needed.
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::seconds(5));

    auto raced = co_await (
        receive_datagrams(socket, server, total_bytes) ||
        timer.async_wait(asio::use_awaitable)
    );

    co_return raced.index() == 0 ? std::get<0>(raced) : (total_bytes > 0);
}

static bool run_benchmark_client_blocking(
    const char* ip, int port, std::uint64_t& total_bytes
) {
    asio::io_context io_context;

    auto result = asio::co_spawn(
        io_context,
        run_benchmark_client(ip, port, total_bytes),
        asio::use_future
    );

    io_context.run();

    return result.get();
}

static void BM_UDP_FileDownload(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;
        const bool ok = run_benchmark_client_blocking(ip, port, downloaded_bytes);

        if (!ok) {
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
