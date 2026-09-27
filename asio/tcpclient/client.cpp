/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file client with Asio
 * Manual smoke-test tool: downloads the served file and writes it to disk, to
 * check by hand that a server works. No campaign runs it; the measured clients
 * are in ../benchmarks/.
 */

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <system_error>

using tcp = asio::ip::tcp;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 65536;

static asio::awaitable<bool> connect_to_server(
    tcp::socket& socket,
    const std::string& server_ip,
    int port
) {
    std::error_code ec;

    tcp::endpoint endpoint(
        asio::ip::make_address(server_ip, ec),
        static_cast<unsigned short>(port)
    );

    if (ec) {
        co_return false;
    }

    co_await socket.async_connect(
        endpoint,
        asio::redirect_error(asio::use_awaitable, ec)
    );

    co_return !ec;
}

static asio::awaitable<bool> receive_file(
    tcp::socket& socket,
    const std::string& output_path
) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        co_return false;
    }

    std::array<char, BUFFER_SIZE> buffer;
    std::uint64_t total_bytes = 0;

    while (true) {
        std::error_code ec;

        const std::size_t n = co_await socket.async_read_some(
            asio::buffer(buffer),
            asio::redirect_error(asio::use_awaitable, ec)
        );

        if (n > 0) {
            out.write(buffer.data(), static_cast<std::streamsize>(n));
            if (!out) {
                std::cerr << "Failed to write output file.\n";
                co_return false;
            }

            total_bytes += static_cast<std::uint64_t>(n);
            continue;
        }

        if (ec == asio::error::eof || ec == asio::error::connection_reset) {
            break;
        }

        if (ec) {
            std::cerr << "async_read_some failed: " << ec.message() << "\n";
            co_return false;
        }

        if (n == 0) {
            break;
        }
    }

    out.close();

    co_return total_bytes > 0;
}

static asio::awaitable<bool> run_client(
    const std::string& server_ip,
    int port,
    const std::string& output_path
) {
    auto executor = co_await asio::this_coro::executor;
    tcp::socket socket(executor);

    if (!(co_await connect_to_server(socket, server_ip, port))) {
        std::cerr << "connect failed\n";
        co_return false;
    }

    co_return co_await receive_file(socket, output_path);
}

static bool run_client_blocking(
    const std::string& server_ip,
    int port,
    const std::string& output_path
) {
    asio::io_context io_context;

    auto result = asio::co_spawn(
        io_context,
        run_client(server_ip, port, output_path),
        asio::use_future
    );

    io_context.run();

    return result.get();
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> [port] [output_file]\n";
        return EXIT_FAILURE;
    }

    const std::string server_ip = argv[1];
    int port = DEFAULT_PORT;
    std::string output_path = "downloaded.bin";

    if (argc >= 3) {
        port = std::stoi(argv[2]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port.\n";
            return EXIT_FAILURE;
        }
    }

    if (argc == 4) {
        output_path = argv[3];
    }

    try {
        const bool ok = run_client_blocking(server_ip, port, output_path);
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}