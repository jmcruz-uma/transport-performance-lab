/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets, BLOCKS model (scenario "blocks", E3).
 * The client consumes the transfer segment by segment as it arrives: a fixed
 * 64 KiB block, touched and discarded, never accumulated into one object.
 * Structurally the same read loop as streaming (E0); kept as its own
 * scenario/binary so all five implementations report a directly comparable
 * "structured, incremental consumption" number, independent of whatever
 * buffer size each E0 baseline happens to use.
 */

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BLOCK_SIZE = 65536;

static int g_port = DEFAULT_PORT;

static int connect_to_server(const std::string& server_ip, int port) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        return -1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) <= 0) {
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
        return -1;
    }

    return sock;
}

static bool receive_blocks(
    int sock,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    total_bytes = 0;

    while (true) {
        const ssize_t n = recv(sock, block.data(), block.size(), 0);

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);

            // "Process" this block in place; nothing accumulates it anywhere.
            benchmark::DoNotOptimize(block.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();

            continue;
        }

        if (n == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        return false;
    }

    return total_bytes > 0;
}

static bool run_benchmark_client(
    const std::string& server_ip,
    int port,
    std::span<char> block,
    std::uint64_t& total_bytes
) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) {
        return false;
    }

    return receive_blocks(sock, block, total_bytes);
}

static void BM_TCP_Blocks(benchmark::State& state) {
    const std::string server_ip = "127.0.0.1";
    const int port = g_port;

    std::array<char, BLOCK_SIZE> block{};
    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;

        if (!run_benchmark_client(
                server_ip,
                port,
                std::span<char>(block.data(), block.size()),
                downloaded_bytes
            )) {
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
