/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio, WHOLE-OBJECT model (scenario "whole_object", E1).
 * The client receives the transfer as ONE object: it accumulates every byte into
 * a single growing buffer (geometric growth), so at end-of-stream the buffer IS
 * the object. No length prefix on the wire (raw-until-close), same as streaming.
 * This is the cost of "hand me the whole thing" for a minimal buffer API.
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
#include <string>
#include <system_error>
#include <vector>

using tcp = asio::ip::tcp;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t READ_CHUNK = 65536;
// Generous up-front estimate of the transfer size. A real client that asks for
// the whole object as one blob would size this from a Content-Length / stat; the
// harness transfers a fixed ~100 MiB file, so 128 MiB reserves enough that the
// accumulation is one pass of copies, not repeated geometric reallocation --
// matching the TAPS arm, whose runtime allocates the final buffer once.
constexpr std::size_t RESERVE_HINT_BYTES = 128ull * 1024 * 1024;

static int g_port = DEFAULT_PORT;

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

static asio::awaitable<bool> receive_whole_object(
    tcp::socket& socket,
    std::uint64_t& total_bytes
) {
    std::vector<char> object;
    object.reserve(RESERVE_HINT_BYTES);  // one pass of copies, not repeated realloc
    std::array<char, READ_CHUNK> chunk{};

    while (true) {
        std::error_code ec;

        const std::size_t n = co_await socket.async_read_some(
            asio::buffer(chunk),
            asio::redirect_error(asio::use_awaitable, ec)
        );

        if (n > 0) {
            object.insert(object.end(), chunk.data(), chunk.data() + n);
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

    total_bytes = object.size();

    // Touch the assembled object so the accumulation cannot be optimised away.
    benchmark::DoNotOptimize(object.data());
    benchmark::DoNotOptimize(total_bytes);
    benchmark::ClobberMemory();

    co_return total_bytes > 0;
}

static asio::awaitable<bool> run_benchmark_client(
    const char* ip,
    int port,
    std::uint64_t& total_bytes
) {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);

    if (!(co_await connect_to_server(socket, ip, port))) {
        co_return false;
    }

    co_return co_await receive_whole_object(socket, total_bytes);
}

static bool run_benchmark_client_blocking(
    const char* ip,
    int port,
    std::uint64_t& total_bytes
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

static void BM_TCP_WholeObject(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
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

BENCHMARK(BM_TCP_WholeObject)
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
