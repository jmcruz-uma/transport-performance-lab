/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * async-berkeley, WHOLE-OBJECT model (scenario "whole_object", E1).
 * The client receives the transfer as ONE object: it accumulates every byte into
 * a single growing buffer (geometric growth), so at end-of-stream the buffer IS
 * the object. No length prefix on the wire (raw-until-close), same as streaming.
 * This is the cost of "hand me the whole thing" for a minimal buffer API.
 *
 * Each recvmsg still reads into a small reused scratch chunk (the sender-based
 * API needs a stable buffer registered before the read completes; growing the
 * accumulator in place would invalidate that pointer on reallocation) and the
 * chunk is copied onto the growing object right after -- same "recv chunk, copy
 * once into the accumulator" shape as every other implementation's E1.
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
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t READ_CHUNK = 65536;
// Generous up-front estimate of the transfer size. A real client that asks for
// the whole object as one blob would size this from a Content-Length / stat; the
// harness transfers a fixed ~100 MiB file, so 128 MiB reserves enough that the
// accumulation is one pass of copies, not repeated geometric reallocation --
// matching the TAPS arm, whose runtime allocates the final buffer once.
constexpr std::size_t RESERVE_HINT_BYTES = 128ull * 1024 * 1024;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

struct BenchmarkState {
    dialog client;
    std::array<char, READ_CHUNK> chunk{};   // reused scratch buffer for each read
    std::vector<char> object;               // grows geometrically; at EOF it is the object
    bool failed = false;

    explicit BenchmarkState(dialog&& socket)
        : client(std::move(socket)) {
        object.reserve(RESERVE_HINT_BYTES);  // one pass of copies, not repeated realloc
    }
};

static constexpr auto error_handler = [](const auto& error) {
    if constexpr (std::is_same_v<std::decay_t<decltype(error)>, int>) {
        std::cerr << std::error_code(error, std::system_category()).message() << "\n";
    } else {
        std::cerr << "async operation failed\n";
    }
};

static void receive_data(
    async_scope& scope,
    std::shared_ptr<BenchmarkState> state
) {
    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        state->chunk.data(),
        state->chunk.size()
    );

    auto operation =
        recvmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_received) {
            if (bytes_received <= 0) {
                // EOF (or an error the sender didn't route to upon_error): the
                // transfer is over either way. Touch the object so the
                // accumulation cannot be optimised away.
                benchmark::DoNotOptimize(state->object.data());
                benchmark::ClobberMemory();
                return;
            }

            state->object.insert(state->object.end(),
                                 state->chunk.data(),
                                 state->chunk.data() + bytes_received);

            receive_data(scope, state);
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
            receive_data(scope, state);
        })
        | upon_error([state](const auto& error) {
            state->failed = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));

    while (trigs.wait()) {
    }

    total_bytes = state->object.size();

    return !state->failed && total_bytes > 0;
}

static void BM_TCP_WholeObject(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
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

BENCHMARK(BM_TCP_WholeObject)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);

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
