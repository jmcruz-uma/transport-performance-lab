/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio, BLOCKS model (scenario "blocks", E3).
 * The client consumes the transfer segment by segment as it arrives: a fixed
 * 64 KiB block, touched and discarded, never accumulated into one object.
 * Structurally the same read loop as streaming (E0); kept as its own
 * scenario/binary so all five implementations report a directly comparable
 * "structured, incremental consumption" number.
 */

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <span>
#include <string>
#include <system_error>

using tcp = asio::ip::tcp;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BLOCK_SIZE = 65536;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

static asio::awaitable<bool> connect_to_server(
    tcp::socket& socket,
    const char* ip,
    int port
) {
    std::error_code ec;

    tcp::endpoint endpoint(
        asio::ip::make_address(ip, ec),
        static_cast<unsigned short>(port)
    );

    if (ec) {
        co_return false;
    }

    co_await socket.async_connect(
        endpoint,
        asio::redirect_error(asio::use_awaitable, ec)
    );

    co_return !ec;
}

static asio::awaitable<bool> receive_blocks(
    tcp::socket& socket,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    total_bytes = 0;

    while (true) {
        std::error_code ec;

        const std::size_t n = co_await socket.async_read_some(
            asio::buffer(block),
            asio::redirect_error(asio::use_awaitable, ec)
        );

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);

            // "Process" this block in place; nothing accumulates it anywhere.
            benchmark::DoNotOptimize(block.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();

            continue;
        }

        if (ec == asio::error::eof || ec == asio::error::connection_reset) {
            break;
        }

        if (ec) {
            co_return false;
        }

        if (n == 0) {
            break;
        }
    }

    co_return total_bytes > 0;
}

static asio::awaitable<bool> run_benchmark_client(
    const char* ip,
    int port,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);

    if (!(co_await connect_to_server(socket, ip, port))) {
        co_return false;
    }

    co_return co_await receive_blocks(socket, block, total_bytes);
}

static bool run_benchmark_client_blocking(
    const char* ip,
    int port,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    asio::io_context io_context;

    auto result = asio::co_spawn(
        io_context,
        run_benchmark_client(ip, port, block, total_bytes),
        asio::use_future
    );

    io_context.run();

    return result.get();
}

static void BM_TCP_Blocks(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::array<char, BLOCK_SIZE> block{};
        std::uint64_t downloaded_bytes = 0;

        const bool ok = run_benchmark_client_blocking(
            ip, port, std::span<char>(block.data(), block.size()), downloaded_bytes
        );

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
