/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file client with Corosio
 * Manual smoke-test tool: downloads the served file and writes it to disk, to
 * check by hand that a server works. No campaign runs it; the measured clients
 * are in ../benchmarks/.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 65536;

static bool is_clean_eof(const std::error_code& ec) {
    if (!ec) {
        return false;
    }

    if (ec == std::errc::connection_reset) {
        return true;
    }

    const std::string message = ec.message();
    return message == "End of file" ||
           message == "end of file" ||
           message == "EOF" ||
           message == "eof";
}

static capy::task<void> run_client(
    corosio::io_context& context,
    const std::string& server_ip,
    int port,
    const std::string& output_path,
    bool& ok
) {
    ok = false;

    corosio::tcp_socket socket(context);
    if (socket.open()) {
        std::cerr << "open failed\n";
        co_return;
    }

    auto [connect_ec] = co_await socket.connect(
        corosio::endpoint(
            corosio::endpoint(server_ip.c_str()),
            static_cast<unsigned short>(port)
        )
    );

    if (connect_ec) {
        std::cerr << "connect failed\n";
        co_return;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        co_return;
    }

    std::array<char, BUFFER_SIZE> buffer;
    std::uint64_t total_bytes = 0;

    while (true) {
        auto [read_ec, n] = co_await socket.read_some(
            capy::mutable_buffer(buffer.data(), buffer.size())
        );

        if (n > 0) {
            total_bytes += static_cast<std::uint64_t>(n);

            out.write(buffer.data(), static_cast<std::streamsize>(n));
            if (!out) {
                std::cerr << "Failed to write output file.\n";
                co_return;
            }

            continue;
        }

        if (!read_ec && n == 0) {
            break;
        }

        if (read_ec) {
            if (total_bytes > 0 && is_clean_eof(read_ec)) {
                break;
            }

            std::cerr << "read failed: " << read_ec.message() << "\n";
            co_return;
        }
    }

    out.close();
    ok = total_bytes > 0;

    co_return;
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

    corosio::io_context context;
    bool ok = false;

    capy::run_async(context.get_executor())(
        run_client(context, server_ip, port, output_path, ok)
    );

    context.run();

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}