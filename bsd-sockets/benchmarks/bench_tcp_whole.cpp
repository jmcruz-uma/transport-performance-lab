/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets, WHOLE-OBJECT model (scenario "whole_object", E1).
 * The client receives the transfer as ONE object: it accumulates every byte into
 * a single growing buffer (geometric growth), so at end-of-stream the buffer IS
 * the object. No length prefix on the wire (raw-until-close), same as streaming.
 * This is the cost of "hand me the whole thing" for a minimal buffer API.
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
#include <string>
#include <vector>

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t READ_CHUNK = 65536;
// Generous up-front estimate of the transfer size. A real client that asks for
// the whole object as one blob would size this from a Content-Length / stat; the
// harness transfers a fixed ~100 MiB file, so 128 MiB reserves enough that the
// accumulation is one pass of copies, not repeated geometric reallocation --
// matching the TAPS arm, whose runtime allocates the final buffer once.
constexpr std::size_t RESERVE_HINT_BYTES = 128ull * 1024 * 1024;

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
        close(sock);
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool receive_whole_object(int sock, std::uint64_t& total_bytes) {
    std::vector<char> object;
    object.reserve(RESERVE_HINT_BYTES);  // one pass of copies, not repeated realloc
    std::array<char, READ_CHUNK> chunk{};

    while (true) {
        const ssize_t n = recv(sock, chunk.data(), chunk.size(), 0);

        if (n > 0) {
            object.insert(object.end(), chunk.data(), chunk.data() + n);
            continue;
        }
        if (n == 0) {
            break;                   // peer closed: the object is complete
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }

    total_bytes = object.size();

    // Touch the assembled object so the accumulation cannot be optimised away.
    benchmark::DoNotOptimize(object.data());
    benchmark::DoNotOptimize(total_bytes);
    benchmark::ClobberMemory();

    return total_bytes > 0;
}

static bool run_benchmark_client(const std::string& server_ip, int port,
                                 std::uint64_t& total_bytes) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) {
        return false;
    }
    const bool ok = receive_whole_object(sock, total_bytes);
    close(sock);
    return ok;
}

static void BM_TCP_WholeObject(benchmark::State& state) {
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
