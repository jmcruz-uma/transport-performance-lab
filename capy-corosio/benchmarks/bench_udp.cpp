/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio UDP file download benchmark (scenarios "udp_k64"/"udp_k1400", E4).
 * Sends one request datagram, then reads datagrams until the server's
 * zero-length end-of-transfer sentinel. UDP gives no delivery guarantee:
 * unlike the TCP scenarios, downloaded_bytes may legitimately fall short of
 * the file size under loss -- that is what this scenario measures, not a bug.
 * OS default socket buffer sizes throughout, like every other scenario.
 *
 * A shared std::stop_source bounds the wait: whichever of {the exchange, a
 * 5s delay} finishes first requests a stop, which cancels the other (an
 * in-flight recv() the same way Corosio's own tests cancel one, or the
 * pending delay()) -- so a lost final sentinel cannot hang the benchmark, and
 * a fast exchange does not have to wait out the full timeout either.
 */

#include <benchmark/benchmark.h>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stop_token>
#include <string>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t RECV_BUFFER_BYTES = 65536;

static int g_port = DEFAULT_PORT;

static capy::task<bool> receive_datagrams(
    corosio::udp_socket& sock, std::uint64_t& total_bytes
) {
    total_bytes = 0;

    auto [sec, sn] = co_await sock.send(capy::const_buffer("r", 1));
    if (sec) {
        co_return false;
    }

    std::array<char, RECV_BUFFER_BYTES> buffer{};

    while (true) {
        auto [ec, n] = co_await sock.recv(
            capy::mutable_buffer(buffer.data(), buffer.size()));

        if (ec) {
            break;  // error, or canceled by the watchdog
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

static bool run_benchmark_client(
    const char* ip, int port, std::uint64_t& total_bytes
) {
    corosio::io_context ctx;
    corosio::udp_socket sock(ctx);

    if (sock.open(corosio::udp::v4())) {
        return false;
    }

    std::stop_source ss;
    bool ok = false;

    auto work = [&]() -> capy::task<void> {
        auto [cec] = co_await sock.connect(
            corosio::endpoint(corosio::ipv4_address(ip), static_cast<std::uint16_t>(port)));
        if (!cec) {
            ok = co_await receive_datagrams(sock, total_bytes);
        }
        ss.request_stop();  // done either way: unblock the watchdog's delay
    };

    auto watchdog = [&]() -> capy::task<void> {
        co_await corosio::delay(std::chrono::seconds(5));
        ss.request_stop();  // timed out: cancels work()'s in-flight recv()
    };

    capy::run_async(ctx.get_executor(), ss.get_token())(work());
    capy::run_async(ctx.get_executor(), ss.get_token())(watchdog());

    ctx.run();

    return ok || total_bytes > 0;
}

static void BM_UDP_FileDownload(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;

        if (!run_benchmark_client(ip, port, downloaded_bytes)) {
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
