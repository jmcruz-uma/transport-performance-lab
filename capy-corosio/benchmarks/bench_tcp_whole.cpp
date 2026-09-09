/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio, WHOLE-OBJECT model (scenario "whole_object", E1).
 * The client receives the transfer as ONE object: it accumulates every byte into
 * a single growing buffer (geometric growth), so at end-of-stream the buffer IS
 * the object. No length prefix on the wire (raw-until-close), same as streaming.
 * This is the cost of "hand me the whole thing" for a minimal buffer API.
 */

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t READ_CHUNK = 65536;
// Generous up-front estimate of the transfer size. A real client that asks for
// the whole object as one blob would size this from a Content-Length / stat; the
// harness transfers a fixed ~100 MiB file, so 128 MiB reserves enough that the
// accumulation is one pass of copies, not repeated geometric reallocation --
// matching the TAPS arm, whose runtime allocates the final buffer once.
constexpr std::size_t RESERVE_HINT_BYTES = 128ull * 1024 * 1024;

static int g_port = DEFAULT_PORT;

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

    std::vector<char> object;
    object.reserve(RESERVE_HINT_BYTES);  // one pass of copies, not repeated realloc
    std::array<char, READ_CHUNK> chunk{};

    while (true) {
        auto [read_ec, n] = co_await socket.read_some(
            capy::mutable_buffer(chunk.data(), chunk.size())
        );

        if (n > 0) {
            object.insert(object.end(), chunk.data(), chunk.data() + n);
            total_bytes = object.size();
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

    total_bytes = object.size();

    // Touch the assembled object so the accumulation cannot be optimised away.
    benchmark::DoNotOptimize(object.data());
    benchmark::DoNotOptimize(total_bytes);
    benchmark::ClobberMemory();

    co_return total_bytes > 0;
}

static void BM_TCP_WholeObject(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        corosio::io_context context;
        std::uint64_t downloaded_bytes = 0;

        auto task = run_benchmark_client(context, ip, port, downloaded_bytes);

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
