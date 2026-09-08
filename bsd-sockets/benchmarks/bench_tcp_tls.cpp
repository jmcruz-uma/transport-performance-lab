/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets raw-byte file download benchmark over TLS 1.3.
 *
 * Identical to bench_tcp.cpp (blocking connect, recv into an 8 KiB buffer, count
 * bytes, stop at EOF) with ONE change: after connect() the fd is wrapped in an
 * OpenSSL client SSL, SSL_connect() runs the handshake (pinned CA, SNI, ALPN,
 * forced cipher/group, TLS 1.3 only) and the read loop uses SSL_read. TLS
 * parameters come from ../../tls/tls_common.hpp.
 */

#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "tls_common.hpp"

constexpr int DEFAULT_PORT = 8080;
// 64 KiB: matches every other arm's application receive buffer (comparability).
// >= the max TLS record (16 KiB); per-call overhead over 100 MB is negligible;
// not tuned, not derived from a kernel setting.
constexpr std::size_t BUFFER_SIZE = 65536;

static int g_port = DEFAULT_PORT;

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
    SSL_set1_host(ssl, tlscommon::kServerName);                 // certificate identity check
    SSL_set_tlsext_host_name(ssl, tlscommon::kServerName);      // SNI

    if (SSL_connect(ssl) != 1) { SSL_free(ssl); return nullptr; }

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) tlscommon::print_tls_identity(ssl, "client");
    return ssl;
}

static bool receive_data(SSL* ssl, std::span<char> buffer, std::uint64_t& total_bytes) {
    total_bytes = 0;
    while (true) {
        const int n = SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);
            benchmark::DoNotOptimize(buffer.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();
            continue;
        }
        const int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_ZERO_RETURN) break;                  // clean close_notify
        if (e == SSL_ERROR_SYSCALL && n == 0) break;            // peer closed without close_notify
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;  // blocking: unlikely
        return false;
    }
    return total_bytes > 0;
}

static bool run_benchmark_client(const std::string& server_ip, int port,
                                 std::span<char> buffer, std::uint64_t& total_bytes) {
    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) return false;
    SSL* ssl = tls_connect(sock);
    if (!ssl) { close(sock); return false; }
    const bool ok = receive_data(ssl, buffer, total_bytes);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sock);
    return ok;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const std::string server_ip = "127.0.0.1";
    const int port = g_port;

    std::array<char, BUFFER_SIZE> buffer{};
    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;
        std::uint64_t downloaded_bytes = 0;
        if (!run_benchmark_client(server_ip, port,
                                  std::span<char>(buffer.data(), buffer.size()),
                                  downloaded_bytes)) {
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
    const std::string prefix = "--server_port=";
    int filtered_argc = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) g_port = std::stoi(arg.substr(prefix.size()));
        else argv[filtered_argc++] = argv[i];
    }
    argv[filtered_argc] = nullptr;

    benchmark::Initialize(&filtered_argc, argv);
    if (benchmark::ReportUnrecognizedArguments(filtered_argc, argv)) return EXIT_FAILURE;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return EXIT_SUCCESS;
}
