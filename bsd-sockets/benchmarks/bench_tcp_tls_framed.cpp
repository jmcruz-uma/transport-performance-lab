/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets file download benchmark over TLS 1.3 with length-prefixed framing
 * (scenario "tls_framed").
 *
 * Like bench_tcp_tls.cpp (blocking connect, SSL_connect, SSL_read loop,
 * TLS_IDENTITY) but the byte stream is a sequence of length-prefixed messages.
 * Deframing is the competent stream-framing loop shared by every non-TAPS arm
 * (../../tls/frame_reader.hpp): SSL_read up to 64 KiB, parse out every complete
 * frame, keep the partial remainder, read more. Bodies are counted as views into
 * the read buffer; nothing is copied. The client reads the shared manifest
 * (tls/manifest.txt, path in TLS_MANIFEST) and fails the run if the received
 * aggregate does not match.
 */

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <atomic>
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
#include "tls_common.hpp"

constexpr int DEFAULT_PORT = 8080;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

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

static SSL_CTX* client_ctx() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (!c || !tlscommon::configure_client_ctx(
                      c, tlscommon::env_or("TLS_CA", "../tls/ca.crt"))) {
            std::exit(EXIT_FAILURE);
        }
        return c;
    }();
    return ctx;
}

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

static SSL* tls_connect(int sock) {
    SSL* ssl = SSL_new(client_ctx());
    if (!ssl) return nullptr;
    SSL_set_fd(ssl, sock);
    SSL_set_connect_state(ssl);
    SSL_set1_host(ssl, tlscommon::kServerName);
    SSL_set_tlsext_host_name(ssl, tlscommon::kServerName);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); return nullptr; }

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) tlscommon::print_tls_identity(ssl, "client");
    return ssl;
}

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
};

static DownloadResult receive_data(SSL* ssl) {
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
        const int n = SSL_read(ssl, space.data(), static_cast<int>(space.size()));
        if (n > 0) {
            fr.committed(static_cast<std::size_t>(n));
            continue;
        }
        const int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && n == 0)) {
            result.ok = !fr.has_partial();  // clean end on a frame boundary
        } else if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            continue;  // blocking socket: unlikely
        }
        break;
    }
    return result;
}

static DownloadResult run_benchmark_client(const std::string& server_ip, int port) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) return DownloadResult{};
    SSL* ssl = tls_connect(sock);
    if (!ssl) { close(sock); return DownloadResult{}; }
    const DownloadResult r = receive_data(ssl);
    SSL_shutdown(ssl);
    SSL_free(ssl);
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
        g_expected = read_manifest(tlscommon::env_or("TLS_MANIFEST", "../tls/manifest.txt"));
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
