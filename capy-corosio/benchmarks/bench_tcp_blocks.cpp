/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio, BLOCKS model (scenario "blocks", E3).
 * The client consumes the transfer segment by segment as it arrives: a fixed
 * 64 KiB block, touched and discarded, never accumulated into one object.
 * Structurally the same read loop as streaming (E0); kept as its own
 * scenario/binary so all five implementations report a directly comparable
 * "structured, incremental consumption" number.
 */

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BLOCK_SIZE = 65536;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

static bool is_clean_eof(const std::error_code& ec) {
    if (!ec) {
        return false;
    }

    if (ec == std::errc::connection_reset) {
        return true;
    }

    const std::string message = ec.message();
    return message == "End of file" ||
           message == "end of file" ||
           message == "EOF" ||
           message == "eof";
}

static capy::task<bool> run_benchmark_client(
    corosio::io_context& context,
    const char* ip,
    int port,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    total_bytes = 0;

    corosio::tcp_socket socket(context);
    if (const auto open_ec = socket.open()) {
        co_return false;
    }

    auto [connect_ec] = co_await socket.connect(
        corosio::endpoint(
            corosio::endpoint(ip),
            static_cast<unsigned short>(port)
        )
    );

    if (connect_ec) {
        co_return false;
    }

    while (true) {
        auto [read_ec, n] = co_await socket.read_some(
            capy::mutable_buffer(block.data(), block.size())
        );

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);

            // "Process" this block in place; nothing accumulates it anywhere.
            benchmark::DoNotOptimize(block.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();

            continue;
        }

        if (!read_ec && n == 0) {
            break;
        }

        if (read_ec) {
            if (total_bytes > 0 && is_clean_eof(read_ec)) {
                break;
            }

            co_return false;
        }
    }

    co_return total_bytes > 0;
}

static void BM_TCP_Blocks(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        corosio::io_context context;
        std::array<char, BLOCK_SIZE> block{};
        std::uint64_t downloaded_bytes = 0;

        auto task = run_benchmark_client(
            context, ip, port, std::span<char>(block.data(), block.size()), downloaded_bytes
        );

        capy::run_async(context.get_executor())(
            std::move(task)
        );

        context.run();

        const bool ok = downloaded_bytes > 0;

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
