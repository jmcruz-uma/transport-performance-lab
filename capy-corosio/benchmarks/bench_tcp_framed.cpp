/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * Corosio file download benchmark, length-prefixed framing, NO security layer
 * (scenario "framed" -- the plaintext mirror of "tls_framed", replacing the
 * old "blocks" scenario -- see ../tcpserver_framed/server.cpp for why).
 *
 * Like bench_tcp.cpp (raw corosio::tcp_socket, read_some loop) but the byte
 * stream is a sequence of length-prefixed messages. Deframing is the same
 * competent stream-framing loop shared by every non-TAPS arm's "tls_framed"
 * client (../../tls/frame_reader.hpp, TLS-agnostic despite the directory):
 * read_some up to 64 KiB, parse out every complete frame, keep the partial
 * remainder, read more. Bodies are counted as views into the read buffer;
 * nothing is copied. The client reads the shared manifest (tls/manifest.txt,
 * path in MANIFEST) and fails the run if the received aggregate does not
 * match.
 */

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>

#include "frame_reader.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

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

static bool is_clean_eof(const std::error_code& ec) {
    if (!ec) return false;
    if (ec == std::errc::connection_reset) return true;
    const std::string m = ec.message();
    return m == "End of file" || m == "end of file" || m == "EOF" || m == "eof";
}

struct DownloadResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    bool ok = false;
};

// result is an out-param (not a return value): a lambda/coroutine capturing
// state by reference and being invoked in the same full-expression risks the
// closure object outliving its captures incorrectly across a suspension
// point, so this follows the same by-reference-parameter shape as
// bench_tcp.cpp's run_benchmark_client(..., total_bytes&).
static capy::task<void> run_benchmark_client(corosio::io_context& context, const char* ip,
                                             int port, DownloadResult& result) {
    corosio::tcp_socket socket(context);
    if (socket.open()) co_return;

    auto [connect_ec] = co_await socket.connect(
        corosio::endpoint(corosio::endpoint(ip), static_cast<unsigned short>(port)));
    if (connect_ec) co_return;

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
        auto [read_ec, n] = co_await socket.read_some(
            capy::mutable_buffer(space.data(), space.size()));

        if (n > 0) {
            fr.committed(static_cast<std::size_t>(n));
            continue;
        }
        if (!read_ec && n == 0) {
            result.ok = !fr.has_partial();
            break;
        }
        if (read_ec) {
            if (result.bytes > 0 && is_clean_eof(read_ec)) {
                result.ok = !fr.has_partial();
            }
            break;
        }
        break;
    }
    co_return;
}

static void BM_TCP_FileDownload(benchmark::State& state) {
    const char* ip = g_server_ip.c_str();
    const int port = g_port;

    std::uint64_t bytes_processed = 0;
    std::uint64_t last_messages = 0;

    for (auto _ : state) {
        (void)_;
        corosio::io_context context;

        DownloadResult r;
        auto task = run_benchmark_client(context, ip, port, r);
        capy::run_async(context.get_executor())(std::move(task));
        context.run();

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
