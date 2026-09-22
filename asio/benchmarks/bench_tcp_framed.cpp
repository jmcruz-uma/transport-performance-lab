/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Asio file download benchmark, length-prefixed framing, NO security layer
 * (scenario "framed" -- the plaintext mirror of "tls_framed", replacing the
 * old "blocks" scenario -- see ../tcpserver_framed/server.cpp for why).
 *
 * Like bench_tcp.cpp (plain tcp::socket) but the byte stream is a sequence of
 * length-prefixed messages. Deframing is the same competent stream-framing
 * loop shared by every non-TAPS arm's "tls_framed" client
 * (../../tls/frame_reader.hpp, TLS-agnostic despite the directory): read up to
 * 64 KiB with one async_read_some, parse out every complete frame, keep the
 * partial remainder, read more -- the same shape taps_cpp's
 * receive_with_framing() runs internally. Bodies are counted as views into
 * the read buffer; nothing is copied. The client reads the shared manifest
 * (tls/manifest.txt, path in MANIFEST) to know how many messages and how many
 * bytes to expect and fails the run if the aggregate does not match.
 */

#include <benchmark/benchmark.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

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

using tcp = asio::ip::tcp;

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

static asio::awaitable<bool> connect_to_server(tcp::socket& socket, const char* ip, int port) {
    std::error_code ec;
    tcp::endpoint endpoint(asio::ip::make_address(ip, ec),
                           static_cast<unsigned short>(port));
    if (ec) co_return false;

    co_await socket.async_connect(endpoint, asio::redirect_error(asio::use_awaitable, ec));
    co_return !ec;
}

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
};

static asio::awaitable<DownloadResult> receive_data(tcp::socket& socket) {
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
        const std::size_t n = co_await socket.async_read_some(
            asio::buffer(space.data(), space.size()),
            asio::redirect_error(asio::use_awaitable, ec));

        if (n > 0) {
            fr.committed(n);
            continue;
        }
        if (ec == asio::error::eof || ec == asio::error::connection_reset) {
            result.ok = !fr.has_partial();
        }
        break;
    }
    co_return result;
}

static asio::awaitable<DownloadResult> run_benchmark_client(const char* ip, int port) {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);
    if (!(co_await connect_to_server(socket, ip, port))) {
        co_return DownloadResult{};
    }
    co_return co_await receive_data(socket);
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
        g_expected = read_manifest(env_or("MANIFEST", "../tls/manifest.txt"));
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
