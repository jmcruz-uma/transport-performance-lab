/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * async-berkeley file download benchmark, length-prefixed framing, NO
 * security layer (scenario "framed" -- the plaintext mirror of "tls_framed"
 * the other four arms have, replacing the old "blocks" scenario -- see
 * ../tcpserver_framed/server.cpp for the full rationale).
 *
 * Like bench_tcp.cpp (recvmsg() continuation loop) but the byte stream is a
 * sequence of length-prefixed messages. Deframing is the same competent
 * stream-framing loop shared by every other "framed"/"tls_framed" client
 * (../../tls/frame_reader.hpp, TLS-agnostic despite the directory): recvmsg
 * up to 64 KiB, parse out every complete frame, keep the partial remainder,
 * read more. Bodies are counted as views into the read buffer; nothing is
 * copied. The client reads the shared manifest (tls/manifest.txt, path in
 * MANIFEST) and fails the run if the received aggregate does not match.
 */

#include <benchmark/benchmark.h>

#include <io/io.hpp>

#include <arpa/inet.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "frame_reader.hpp"

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

constexpr int DEFAULT_PORT = 8080;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : fallback;
}

struct ManifestExpectation {
    std::uint64_t count = 0;
    std::uint64_t total_bytes = 0;
};

static ManifestExpectation read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    ManifestExpectation e;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') {
            continue;
        }
        e.total_bytes += std::stoull(line.substr(b));
        ++e.count;
    }
    if (e.count == 0) {
        throw std::runtime_error("manifest has no message sizes: " + path);
    }
    return e;
}

static ManifestExpectation g_expected;

struct BenchmarkState {
    dialog client;
    tlsframe::FrameReader fr;
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool failed = false;
    bool ok = false;

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

static void receive_data(
    async_scope& scope,
    std::shared_ptr<BenchmarkState> state
) {
    while (auto body = state->fr.next_frame()) {
        ++state->messages;
        state->bytes += static_cast<std::uint64_t>(body->size());

        benchmark::DoNotOptimize(body->data());
        benchmark::DoNotOptimize(state->bytes);
        benchmark::ClobberMemory();
    }

    const std::span<char> space = state->fr.read_span();
    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(space.data(), space.size());

    auto operation =
        recvmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_received) {
            if (bytes_received > 0) {
                state->fr.committed(static_cast<std::size_t>(bytes_received));
                receive_data(scope, state);
                return;
            }
            // bytes_received == 0: clean end-of-stream, same convention
            // bench_tcp.cpp's plain client relies on.
            state->ok = !state->fr.has_partial();
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
    std::uint64_t& messages,
    std::uint64_t& bytes
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

    messages = state->messages;
    bytes = state->bytes;

    return !state->failed && state->ok && bytes > 0;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_messages = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t messages = 0;
        std::uint64_t bytes = 0;

        if (!run_benchmark_client(ip, port, messages, bytes)) {
            state.SkipWithError("Download failed.");
            break;
        }
        if (messages != g_expected.count || bytes != g_expected.total_bytes) {
            state.SkipWithError("Received aggregate does not match the manifest.");
            break;
        }

        bytes_processed += bytes;
        last_messages = messages;
    }

    state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));
    state.counters["messages"] = static_cast<double>(last_messages);
}

BENCHMARK(BM_TCP_FileDownload)
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

    try {
        g_expected = read_manifest(env_or("MANIFEST", "../tls/manifest.txt"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    benchmark::Initialize(&filtered_argc, argv);

    if (benchmark::ReportUnrecognizedArguments(filtered_argc, argv)) {
        return EXIT_FAILURE;
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    return EXIT_SUCCESS;
}
