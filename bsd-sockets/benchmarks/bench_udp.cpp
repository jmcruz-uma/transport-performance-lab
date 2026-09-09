/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets UDP file download benchmark (scenarios "udp_k64"/"udp_k1400", E4).
 * Sends one request datagram, then reads datagrams until the server's
 * zero-length end-of-transfer sentinel (or a receive timeout). UDP gives no
 * delivery guarantee: unlike the TCP scenarios, downloaded_bytes may
 * legitimately fall short of the file size under loss -- that is what this
 * scenario measures, not a bug. OS default socket buffer sizes throughout, on
 * both ends, like every other scenario.
 */

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t RECV_BUFFER_BYTES = 65536;
// Wall-clock bound on the whole receive, matching every other arm's UDP client
// (asio / taps / corosio race a 5 s timer against the exchange; async-berkeley
// polls to a 5 s deadline). SO_RCVTIMEO below is set short, only as a poll
// granularity so the deadline can be checked; it is NOT a 5 s idle timeout.
constexpr int RECV_TIMEOUT_SECONDS = 5;
constexpr int RECV_POLL_MS = 200;

static int g_port = DEFAULT_PORT;

static int connect_to_server(const std::string& server_ip, int port) {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        return -1;
    }

    timeval tv{};
    tv.tv_usec = RECV_POLL_MS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool receive_datagrams(int sock, std::uint64_t& total_bytes) {
    total_bytes = 0;

    // Request datagram: content is irrelevant, only the source address matters.
    const char request = 'r';
    if (send(sock, &request, sizeof(request), 0) == -1) {
        return false;
    }

    std::array<char, RECV_BUFFER_BYTES> buffer{};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(RECV_TIMEOUT_SECONDS);

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
            break;  // zero-length datagram: end-of-transfer sentinel
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (std::chrono::steady_clock::now() < deadline) {
                continue;  // poll slice elapsed, deadline not reached: keep waiting
            }
            break;  // sentinel lost: stop with whatever arrived so far
        }

        // Real error: stop here with whatever arrived so far.
        break;
    }

    return total_bytes > 0;
}

static bool run_benchmark_client(
    const std::string& server_ip,
    int port,
    std::uint64_t& total_bytes
) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) {
        return false;
    }

    const bool ok = receive_datagrams(sock, total_bytes);
    close(sock);
    return ok;
}

static void BM_UDP_FileDownload(benchmark::State& state) {
    const std::string server_ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;

        std::uint64_t downloaded_bytes = 0;

        if (!run_benchmark_client(server_ip, port, downloaded_bytes)) {
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
