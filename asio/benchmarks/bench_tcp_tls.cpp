/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio raw-byte file download benchmark over TLS 1.3.
 *
 * Identical to bench_tcp.cpp (streaming: read the byte-stream as it arrives into
 * a 64 KiB buffer, count bytes, stop at EOF) with ONE change: the socket is
 * wrapped in an asio::ssl::stream and a client handshake -- pinned CA, SNI, ALPN,
 * forced cipher/group, TLS 1.3 only -- runs first. TLS parameters come from
 * ../../tls/tls_common.hpp.
 */

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <span>
#include <string>
#include <system_error>

#include "tls_common.hpp"

using tcp = asio::ip::tcp;
using tls_stream = asio::ssl::stream<tcp::socket>;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 65536;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

static asio::ssl::context& client_ctx() {
    static asio::ssl::context ctx = [] {
        asio::ssl::context c(asio::ssl::context::tls_client);
        if (!tlscommon::configure_client_ctx(
                c.native_handle(), tlscommon::env_or("TLS_CA", "../tls/ca.crt"))) {
            std::exit(EXIT_FAILURE);
        }
        return c;
    }();
    return ctx;
}

static asio::awaitable<bool> connect_and_handshake(tls_stream& stream, const char* ip, int port) {
    std::error_code ec;
    tcp::endpoint endpoint(asio::ip::make_address(ip, ec),
                           static_cast<unsigned short>(port));
    if (ec) co_return false;

    co_await stream.next_layer().async_connect(
        endpoint, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return false;

    stream.set_verify_callback(asio::ssl::host_name_verification(tlscommon::kServerName), ec);
    if (ec) co_return false;
    if (SSL_set_tlsext_host_name(stream.native_handle(), tlscommon::kServerName) != 1)
        co_return false;

    co_await stream.async_handshake(asio::ssl::stream_base::client,
                                   asio::redirect_error(asio::use_awaitable, ec));
    if (ec) co_return false;

    static std::atomic<bool> printed{false};
    if (!printed.exchange(true)) {
        tlscommon::print_tls_identity(stream.native_handle(), "client");
    }
    co_return true;
}

static asio::awaitable<bool> receive_data(tls_stream& stream, std::span<char> buffer,
                                          std::uint64_t& total_bytes) {
    total_bytes = 0;
    while (true) {
        std::error_code ec;
        const std::size_t n = co_await stream.async_read_some(
            asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, ec));

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);
            benchmark::DoNotOptimize(buffer.data());
            benchmark::DoNotOptimize(total_bytes);
            benchmark::ClobberMemory();
            continue;
        }
        if (ec == asio::error::eof || ec == asio::ssl::error::stream_truncated ||
            ec == asio::error::connection_reset) {
            break;
        }
        if (ec) co_return false;
        if (n == 0) break;
    }
    co_return total_bytes > 0;
}

static asio::awaitable<bool> run_benchmark_client(const char* ip, int port,
                                                  std::span<char> buffer,
                                                  std::uint64_t& total_bytes) {
    auto executor = co_await asio::this_coro::executor;
    tls_stream stream(tcp::socket(executor), client_ctx());
    if (!(co_await connect_and_handshake(stream, ip, port))) co_return false;
    co_return co_await receive_data(stream, buffer, total_bytes);
}

static bool run_benchmark_client_blocking(const char* ip, int port, std::span<char> buffer,
                                          std::uint64_t& total_bytes) {
    asio::io_context io_context;
    auto result = asio::co_spawn(io_context,
                                 run_benchmark_client(ip, port, buffer, total_bytes),
                                 asio::use_future);
    io_context.run();
    return result.get();
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_downloaded_bytes = 0;

    for (auto _ : state) {
        (void)_;
        std::array<char, BUFFER_SIZE> buffer{};
        std::uint64_t downloaded_bytes = 0;

        const bool ok = run_benchmark_client_blocking(
            ip, port, std::span<char>(buffer.data(), buffer.size()), downloaded_bytes);

        if (!ok) {
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
