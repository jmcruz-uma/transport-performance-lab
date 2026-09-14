/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets raw-byte file download benchmark
 * Minimal benchmark version for performance and energy measurements.
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
// 64 KiB: the application receive buffer size used by every other arm, so the
// comparison holds this constant. It is >= the maximum TLS record (16 KiB) and
// large enough that per-call overhead over a 100 MB transfer is negligible; it is
// not a tuned parameter and not derived from any kernel setting.
constexpr std::size_t BUFFER_SIZE = 65536;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

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

static bool receive_data(
    int sock,
    std::span<char> buffer,
    std::uint64_t& total_bytes
) {
    total_bytes = 0;

    while (true) {
        const ssize_t n = recv(sock, buffer.data(), buffer.size(), 0);

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);

            benchmark::DoNotOptimize(buffer.data());
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
    std::span<char> buffer,
    std::uint64_t& total_bytes
) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) {
        return false;
    }

    return receive_data(sock, buffer, total_bytes);
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const std::string& server_ip = g_server_ip;
    const int port = g_port;

    std::array<char, BUFFER_SIZE> buffer{};
    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;

        if (!run_benchmark_client(
                server_ip,
                port,
                std::span<char>(buffer.data(), buffer.size()),
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

BENCHMARK(BM_TCP_FileDownload)
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