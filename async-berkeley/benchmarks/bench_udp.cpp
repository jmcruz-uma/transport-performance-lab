/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * async-berkeley UDP file download benchmark (scenarios "udp_k64"/"udp_k1400",
 * E4). Sends one request datagram, then reads datagrams until the server's
 * zero-length end-of-transfer sentinel. UDP gives no delivery guarantee:
 * unlike the TCP scenarios, downloaded_bytes may legitimately fall short of
 * the file size under loss -- that is what this scenario measures, not a bug.
 * OS default socket buffer sizes throughout, like every other scenario.
 */

#include <benchmark/benchmark.h>

#include <io/io.hpp>

#include <arpa/inet.h>

#include <array>
#include <cerrno>
#include <chrono>
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

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t RECV_BUFFER_BYTES = 65536;
// Same wall-clock bound on the receive as every other arm's UDP client (asio /
// taps / corosio use a 5 s timer race; bsd a 5 s SO_RCVTIMEO): if the server's
// zero-length sentinel is lost, stop with whatever arrived instead of hanging.
constexpr int RECV_TIMEOUT_SECONDS = 5;

static int g_port = DEFAULT_PORT;

struct BenchmarkState {
    dialog client;
    std::array<char, RECV_BUFFER_BYTES> buffer{};
    std::uint64_t total_bytes = 0;
    bool failed = false;
    bool done = false;

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

static void receive_datagrams(
    async_scope& scope,
    std::shared_ptr<BenchmarkState> state
) {
    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        state->buffer.data(),
        state->buffer.size()
    );

    auto operation =
        recvmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_received) {
            if (bytes_received <= 0) {
                state->done = true;  // zero-length sentinel: transfer is over
                return;
            }

            state->total_bytes += static_cast<std::uint64_t>(bytes_received);

            benchmark::DoNotOptimize(state->buffer.data());
            benchmark::DoNotOptimize(state->total_bytes);
            benchmark::ClobberMemory();

            receive_datagrams(scope, state);
        })
        | upon_error([state, msg](const auto& error) {
            state->failed = true;
            state->done = true;
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

    auto client = trigs.emplace(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    auto server_address = make_address<sockaddr_in>();
    server_address->sin_family = AF_INET;
    server_address->sin_port = htons(static_cast<std::uint16_t>(port));
    server_address->sin_addr.s_addr = inet_addr(ip);

    auto state = std::make_shared<BenchmarkState>(std::move(client));

    auto operation =
        io::connect(state->client, server_address)
        | then([&scope, state](const auto&) {
            // Request datagram: content is irrelevant, only the source
            // address the server observes matters.
            static char request_byte = 'r';
            auto req = std::make_shared<message>();
            req->buffers.emplace_back(&request_byte, 1);

            auto send_op =
                sendmsg(state->client, *req, 0)
                | then([&scope, state, req](ssize_t) {
                    receive_datagrams(scope, state);
                })
                | upon_error([state, req](const auto& error) {
                    state->failed = true;
                    state->done = true;
                    error_handler(error);
                });

            scope.spawn(std::move(send_op));
        })
        | upon_error([state](const auto& error) {
            state->failed = true;
            state->done = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(RECV_TIMEOUT_SECONDS);
    while (!state->done) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;  // sentinel lost: stop with whatever arrived so far
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (trigs.wait_for(static_cast<int>(remaining > 0 ? remaining : 1)) == 0) {
            break;  // poll timed out, or nothing left pending
        }
    }

    total_bytes = state->total_bytes;

    return !state->failed && total_bytes > 0;
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
