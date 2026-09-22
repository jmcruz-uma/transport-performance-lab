/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets file download benchmark, length-prefixed framing, NO security
 * layer (scenario "framed" -- the plaintext mirror of "tls_framed", replacing
 * the old "blocks" scenario -- see ../tcpserver_framed/server.cpp for why).
 *
 * Like bench_tcp.cpp (blocking connect, recv() loop) but the byte stream is a
 * sequence of length-prefixed messages. Deframing is the same competent
 * stream-framing loop shared by every non-TAPS arm's "tls_framed" client
 * (../../tls/frame_reader.hpp, TLS-agnostic despite the directory): recv() up
 * to 64 KiB, parse out every complete frame, keep the partial remainder, read
 * more. Bodies are counted as views into the read buffer; nothing is copied.
 * The client reads the shared manifest (tls/manifest.txt, path in MANIFEST)
 * and fails the run if the received aggregate does not match.
 */

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "frame_reader.hpp"

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
    if (!in) throw std::runtime_error("cannot open manifest: " + path);
    ManifestExpectation e;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        e.total_bytes += std::stoull(line.substr(b));
        ++e.count;
    }
    if (e.count == 0) throw std::runtime_error("manifest has no message sizes: " + path);
    return e;
}

static ManifestExpectation g_expected;

static int connect_to_server(const std::string& server_ip, int port) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return -1;
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) <= 0) { close(sock); return -1; }
    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) { close(sock); return -1; }
    return sock;
}

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
};

static DownloadResult receive_data(int sock) {
    DownloadResult result;
    tlsframe::FrameReader fr;

    while (true) {
        while (auto body = fr.next_frame()) {
            ++result.messages;
            result.bytes += body->size();
            benchmark::DoNotOptimize(body->data());
            benchmark::DoNotOptimize(result.bytes);
            benchmark::ClobberMemory();
        }

        const std::span<char> space = fr.read_span();
        const ssize_t n = recv(sock, space.data(), space.size(), 0);
        if (n > 0) {
            fr.committed(static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            result.ok = !fr.has_partial();  // clean end on a frame boundary
        } else if (errno == EINTR) {
            continue;
        }
        break;
    }
    return result;
}

static DownloadResult run_benchmark_client(const std::string& server_ip, int port) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) return DownloadResult{};
    const DownloadResult r = receive_data(sock);
    close(sock);
    return r;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const std::string& server_ip = g_server_ip;
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_messages = 0;

    for (auto _ : state) {
        (void)_;
        const DownloadResult r = run_benchmark_client(server_ip, port);

        if (!r.ok || r.bytes == 0) {
            state.SkipWithError("Download failed.");
            break;
        }
        if (r.messages != g_expected.count || r.bytes != g_expected.total_bytes) {
            state.SkipWithError("Received aggregate does not match the manifest.");
            break;
        }
        bytes_processed += r.bytes;
        last_messages = r.messages;
    }

    state.SetBytesProcessed(static_cast<int64_t>(bytes_processed));
    state.counters["messages"] = static_cast<double>(last_messages);
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
        if (arg.rfind(port_prefix, 0) == 0) g_port = std::stoi(arg.substr(port_prefix.size()));
        else if (arg.rfind(ip_prefix, 0) == 0) g_server_ip = arg.substr(ip_prefix.size());
        else argv[filtered_argc++] = argv[i];
    }
    argv[filtered_argc] = nullptr;

    try {
        g_expected = read_manifest(env_or("MANIFEST", "../tls/manifest.txt"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    benchmark::Initialize(&filtered_argc, argv);
    if (benchmark::ReportUnrecognizedArguments(filtered_argc, argv)) return EXIT_FAILURE;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return EXIT_SUCCESS;
}
