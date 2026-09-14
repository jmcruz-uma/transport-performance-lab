/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio file download benchmark over TLS 1.3 with length-prefixed framing
 * (scenario "tls_framed").
 *
 * Like bench_tcp_tls.cpp (asio::ssl::stream, client handshake, TLS_IDENTITY)
 * but the byte stream is a sequence of length-prefixed messages. Deframing is
 * the competent stream-framing loop shared by every non-TAPS arm
 * (../../tls/frame_reader.hpp): read up to 64 KiB with one async_read_some, parse
 * out every complete frame, keep the partial remainder, read more -- the same
 * shape taps_cpp's receive_with_framing() runs internally. Bodies are counted as
 * views into the read buffer; nothing is copied. The client reads the shared
 * manifest (tls/manifest.txt, path in TLS_MANIFEST) to know how many messages
 * and how many bytes to expect and fails the run if the aggregate does not match.
 */

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>

#include "frame_reader.hpp"
#include "tls_common.hpp"

using tcp = asio::ip::tcp;
using tls_stream = asio::ssl::stream<tcp::socket>;

constexpr int DEFAULT_PORT = 8080;

static int g_port = DEFAULT_PORT;
static std::string g_server_ip = "127.0.0.1";

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

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
};

static asio::awaitable<DownloadResult> receive_data(tls_stream& stream) {
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

        std::error_code ec;
        const std::span<char> space = fr.read_span();
        const std::size_t n = co_await stream.async_read_some(
            asio::buffer(space.data(), space.size()),
            asio::redirect_error(asio::use_awaitable, ec));

        if (n > 0) {
            fr.committed(n);
            continue;
        }
        if (ec == asio::error::eof || ec == asio::ssl::error::stream_truncated ||
            ec == asio::error::connection_reset) {
            result.ok = !fr.has_partial();
        }
        break;
    }
    co_return result;
}

static asio::awaitable<DownloadResult> run_benchmark_client(const char* ip, int port) {
    auto executor = co_await asio::this_coro::executor;
    tls_stream stream(tcp::socket(executor), client_ctx());
    if (!(co_await connect_and_handshake(stream, ip, port))) {
        co_return DownloadResult{};
    }
    co_return co_await receive_data(stream);
}

static DownloadResult run_benchmark_client_blocking(const char* ip, int port) {
    asio::io_context io_context;
    auto result = asio::co_spawn(io_context, run_benchmark_client(ip, port), asio::use_future);
    io_context.run();
    return result.get();
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_messages = 0;

    for (auto _ : state) {
        (void)_;
        const DownloadResult r = run_benchmark_client_blocking(ip, port);

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
        g_expected = read_manifest(tlscommon::env_or("TLS_MANIFEST", "../tls/manifest.txt"));
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
