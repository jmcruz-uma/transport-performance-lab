/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * async-berkeley, BLOCKS model (scenario "blocks", E3).
 * The client consumes the transfer segment by segment as it arrives: a fixed
 * 64 KiB block, touched and discarded, never accumulated into one object.
 * Structurally the same read loop as streaming (E0); kept as its own
 * scenario/binary so all five implementations report a directly comparable
 * "structured, incremental consumption" number.
 */

#include <benchmark/benchmark.h>

#include <io/io.hpp>

#include <arpa/inet.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BLOCK_SIZE = 65536;

static int g_port = DEFAULT_PORT;

struct BenchmarkState {
    dialog client;
    std::array<char, BLOCK_SIZE> block{};   // reused scratch buffer for each read
    std::uint64_t total_bytes = 0;
    bool failed = false;

    explicit BenchmarkState(dialog&& socket)
        : client(std::move(socket)) {
    }
};

static constexpr auto error_handler = [](const auto& error) {
    if constexpr (std::is_same_v<std::decay_t<decltype(error)>, int>) {
        std::cerr << std::error_code(error, std::system_category()).message() << "\n";
    } else {
        std::cerr << "async operation failed\n";
    }
};

static void receive_blocks(
    async_scope& scope,
    std::shared_ptr<BenchmarkState> state
) {
    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        state->block.data(),
        state->block.size()
    );

    auto operation =
        recvmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_received) {
            if (bytes_received <= 0) {
                return;
            }

            state->total_bytes += static_cast<std::uint64_t>(bytes_received);

            // "Process" this block in place; nothing accumulates it anywhere.
            benchmark::DoNotOptimize(state->block.data());
            benchmark::DoNotOptimize(state->total_bytes);
            benchmark::ClobberMemory();

            receive_blocks(scope, state);
        })
        | upon_error([state, msg](const auto& error) {
            state->failed = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

static bool run_benchmark_client(
    const char* ip,
    int port,
    std::uint64_t& total_bytes
) {
    async_scope scope;
    triggers trigs;

    auto client = trigs.emplace(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    auto server_address = make_address<sockaddr_in>();
    server_address->sin_family = AF_INET;
    server_address->sin_port = htons(static_cast<std::uint16_t>(port));
    server_address->sin_addr.s_addr = inet_addr(ip);

    auto state = std::make_shared<BenchmarkState>(std::move(client));

    auto operation =
        io::connect(state->client, server_address)
        | then([&scope, state](const auto&) {
            receive_blocks(scope, state);
        })
        | upon_error([state](const auto& error) {
            state->failed = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));

    while (trigs.wait()) {
    }

    total_bytes = state->total_bytes;

    return !state->failed && total_bytes > 0;
}

static void BM_TCP_Blocks(benchmark::State& state) {
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

BENCHMARK(BM_TCP_Blocks)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

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
