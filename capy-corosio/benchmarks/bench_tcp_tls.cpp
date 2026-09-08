/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio raw-byte file download benchmark over TLS 1.3.
 *
 * Identical to bench_tcp.cpp (read the byte-stream as it arrives into a 64 KiB
 * buffer, count bytes, stop at EOF) with ONE change: the socket is wrapped in a
 * corosio::openssl_stream and a client handshake -- pinned CA, SNI, ALPN, pinned
 * TLS 1.3 cipher, TLS 1.3 only -- runs first. See the comparability note in
 * ../tcpserver_tls/server.cpp about the X25519 group. Values from
 * ../../tls/tls_common.hpp.
 */

#include <benchmark/benchmark.h>

#include <openssl/crypto.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tls_context.hpp>

#include "tls_common.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 65536;

static int g_port = DEFAULT_PORT;

static corosio::tls_context& client_ctx() {
    static corosio::tls_context ctx = [] {
        corosio::tls_context c;
        if (c.load_verify_file(tlscommon::env_or("TLS_CA", "../tls/ca.crt")) ||
            c.set_verify_mode(corosio::tls_verify_mode::peer) ||
            c.set_min_protocol_version(corosio::tls_version::tls_1_3) ||
            c.set_max_protocol_version(corosio::tls_version::tls_1_3) ||
            c.set_ciphersuites_tls13(tlscommon::kCipherSuite) ||
            c.set_alpn({tlscommon::kAlpn})) {
            std::fprintf(stderr, "tls: client context configuration failed\n");
            std::exit(EXIT_FAILURE);
        }
        return c;
    }();
    return ctx;
}

static void print_identity(std::string_view alpn) {
    std::fprintf(stderr,
                 "TLS_IDENTITY who=client openssl=\"%s\" version=TLSv1.3 cipher=%s alpn=%.*s\n",
                 OpenSSL_version(OPENSSL_VERSION), tlscommon::kCipherSuite,
                 static_cast<int>(alpn.size()), alpn.data());
}

static bool is_clean_eof(const std::error_code& ec) {
    if (!ec) return false;
    if (ec == std::errc::connection_reset) return true;
    const std::string m = ec.message();
    return m == "End of file" || m == "end of file" || m == "EOF" || m == "eof";
}

static capy::task<bool> run_benchmark_client(corosio::io_context& context, const char* ip,
                                             int port, std::span<char> buffer,
                                             std::uint64_t& total_bytes) {
    total_bytes = 0;

    corosio::tcp_socket socket(context);
    if (socket.open()) co_return false;

    auto [connect_ec] = co_await socket.connect(
        corosio::endpoint(corosio::endpoint(ip), static_cast<unsigned short>(port)));
    if (connect_ec) co_return false;

    corosio::openssl_stream tls(std::move(socket), client_ctx());
    tls.set_hostname(tlscommon::kServerName);

    auto [hs_ec] = co_await tls.handshake(corosio::tls_role::client);
    if (hs_ec) co_return false;

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) print_identity(tls.alpn_protocol());

    while (true) {
        auto [read_ec, n] = co_await tls.read_some(
            capy::mutable_buffer(buffer.data(), buffer.size()));

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);
            benchmark::DoNotOptimize(buffer.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();
            continue;
        }
        if (!read_ec && n == 0) break;
        if (read_ec) {
            if (total_bytes > 0 && is_clean_eof(read_ec)) break;
            co_return false;
        }
    }

    auto [sd_ec] = co_await tls.shutdown();
    (void)sd_ec;
    co_return total_bytes > 0;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    constexpr const char* ip = "127.0.0.1";
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;
        corosio::io_context context;
        std::array<char, BUFFER_SIZE> buffer{};
        std::uint64_t downloaded_bytes = 0;

        auto task = run_benchmark_client(context, ip, port,
                                         std::span<char>(buffer.data(), buffer.size()),
                                         downloaded_bytes);
        capy::run_async(context.get_executor())(std::move(task));
        context.run();

        if (downloaded_bytes == 0) {
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
