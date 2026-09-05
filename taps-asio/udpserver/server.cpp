/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TAPS UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * Uses TAPS's own Listener/Connection abstraction (RELIABILITY::AVOID selects
 * UDP): each accept() gives one Connection per detected source, sharing the
 * listener's underlying socket -- the library's own UDPListener already
 * serializes access to it internally, so this file never touches raw sockets
 * or concurrency primitives. On each accepted connection, the whole file is
 * sent back as a sequence of datagrams (DGRAM_BYTES, default the max IPv4 UDP
 * payload, 65507 bytes), followed by one empty Message -- the end-of-transfer
 * sentinel, since UDP has no end-of-stream of its own.
 *
 * Deliberately uses the OS's default socket buffer sizes, like every other
 * scenario: any loss it causes at these datagram sizes is a real, comparable
 * measurement, not an artifact of tuning some implementations and not others
 * (and TAPS's own UDP path does not currently expose a way to tune them
 * anyway -- see the design notes on this scenario).
 */

#include "taps/taps_api.h"

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;
// 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
// payload a single send() can carry; anything above it fails.
constexpr std::size_t DEFAULT_DGRAM_BYTES = 65507;

static std::size_t dgram_bytes() {
    if (const char* s = std::getenv("DGRAM_BYTES")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v > 0) {
            return static_cast<std::size_t>(v);
        }
    }
    return DEFAULT_DGRAM_BYTES;
}

static asio::awaitable<void> serve_connection(
    std::unique_ptr<taps::Connection> conn,
    std::shared_ptr<std::vector<std::uint8_t>> payload,
    std::size_t dgram_size
) {
    // The request datagram's content is irrelevant; receiving it is what
    // gives PassiveUDPConnection this client's address to reply to.
    auto request = co_await conn->receive();
    if (!request) {
        co_return;
    }

    std::size_t sent = 0;
    while (sent < payload->size()) {
        const std::size_t n = std::min(dgram_size, payload->size() - sent);
        auto view = taps::make_message_view(
            std::span<const std::uint8_t>(payload->data() + sent, n));
        auto r = co_await conn->send(view);
        if (!r) {
            co_return;
        }
        sent += n;
    }

    // Zero-length Message: end-of-transfer sentinel.
    auto sentinel = taps::make_message_view(std::span<const std::uint8_t>{});
    (void)co_await conn->send(sentinel);

    co_await conn->close();
}

static asio::awaitable<void> accept_loop(
    std::unique_ptr<taps::Listener> listener,
    std::shared_ptr<std::vector<std::uint8_t>> payload,
    std::size_t dgram_size
) {
    auto executor = co_await asio::this_coro::executor;
    for (;;) {
        auto a = co_await listener->accept();
        if (!a) {
            continue;
        }
        asio::co_spawn(
            executor,
            serve_connection(std::move(*a), payload, dgram_size),
            asio::detached);
    }
}

static asio::awaitable<void> run_server(
    asio::io_context& io_context, int port,
    std::shared_ptr<std::vector<std::uint8_t>> payload,
    std::size_t dgram_size
) {
    taps::TransportServices ts(io_context);
    taps::TransportProperties props;
    props.set(taps::PropertyKey::RELIABILITY, taps::SelectionProperty::AVOID);

    auto lr = co_await ts.listen(
        taps::LocalEndpoint{"0.0.0.0", static_cast<std::uint16_t>(port)}, std::move(props));
    if (!lr) {
        std::cerr << "listen failed: " << lr.error().message() << "\n";
        co_return;
    }

    co_await accept_loop(std::move(*lr), payload, dgram_size);
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <file_path> [port] [threads]\n";
        return EXIT_FAILURE;
    }

    const fs::path file_path = argv[1];
    int port = DEFAULT_PORT;
    int threads = 1;

    if (argc >= 3) {
        port = std::stoi(argv[2]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port.\n";
            return EXIT_FAILURE;
        }
    }

    if (argc == 4) {
        threads = std::stoi(argv[3]);
        if (threads <= 0 || threads > MAX_THREADS) {
            std::cerr << "Invalid thread count.\n";
            return EXIT_FAILURE;
        }
    }

    if (!fs::exists(file_path) || !fs::is_regular_file(file_path)) {
        std::cerr << "Input path is not a regular file: " << file_path << "\n";
        return EXIT_FAILURE;
    }

    auto payload = std::make_shared<std::vector<std::uint8_t>>();
    {
        std::ifstream in(file_path, std::ios::binary);
        payload->assign(std::istreambuf_iterator<char>(in), {});
    }

    const std::size_t dgram_size = dgram_bytes();

    asio::io_context io_context;
    auto work_guard = asio::make_work_guard(io_context);

    asio::co_spawn(io_context, run_server(io_context, port, payload, dgram_size), asio::detached);

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&io_context] { io_context.run(); });
    }
    for (auto& worker : pool) {
        worker.join();
    }

    return EXIT_SUCCESS;
}
