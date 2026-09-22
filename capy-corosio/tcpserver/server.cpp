/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with Corosio
 * Minimal raw-byte server for performance and energy measurements.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio.hpp>

namespace fs = std::filesystem;
namespace corosio = boost::corosio;
namespace capy = boost::capy;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr int MAX_THREADS = 256;

struct FileMapping {
    int fd = -1;
    const char* data = nullptr;
    std::size_t size = 0;
};

static FileMapping map_file_read_only(const fs::path& path) {
    FileMapping mapping{};

    const std::uintmax_t file_size = fs::file_size(path);
    mapping.size = static_cast<std::size_t>(file_size);

    mapping.fd = open(path.c_str(), O_RDONLY);
    if (mapping.fd == -1) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    if (mapping.size == 0) {
        mapping.data = nullptr;
        return mapping;
    }

    void* ptr = mmap(nullptr, mapping.size, PROT_READ, MAP_PRIVATE, mapping.fd, 0);
    if (ptr == MAP_FAILED) {
        close(mapping.fd);
        throw std::runtime_error("mmap failed.");
    }

    mapping.data = static_cast<const char*>(ptr);
    return mapping;
}

static void unmap_file(FileMapping& mapping) {
    if (mapping.data != nullptr && mapping.size > 0) {
        munmap(const_cast<char*>(mapping.data), mapping.size);
    }

    if (mapping.fd != -1) {
        close(mapping.fd);
    }

    mapping.data = nullptr;
    mapping.fd = -1;
    mapping.size = 0;
}

static capy::task<void> send_file(
    corosio::tcp_socket& sock,
    std::span<const char> file_view
) {
    std::size_t sent = 0;

    while (sent < file_view.size()) {
        auto [ec, n] = co_await sock.write_some(
            capy::const_buffer(file_view.data() + sent, file_view.size() - sent)
        );

        if (ec || n == 0) {
            break;
        }

        sent += static_cast<std::size_t>(n);
    }

    co_return;
}

static capy::task<void> serve_client(
    corosio::tcp_socket sock,
    std::span<const char> file_view
) {
    co_await send_file(sock, file_view);
    co_return;
}

static capy::task<void> accept_loop(
    corosio::io_context& ctx,
    corosio::tcp_acceptor& acceptor,
    std::span<const char> file_view
) {
    while (true) {
        corosio::tcp_socket sock(ctx);
        auto [ec] = co_await acceptor.accept(sock);

        if (ec) {
            continue;
        }

        capy::run_async(ctx.get_executor())(
            serve_client(std::move(sock), file_view)
        );
    }
}

static void run_io_context(corosio::io_context& ctx) {
    ctx.run();
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

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

    FileMapping mapping{};
    try {
        mapping = map_file_read_only(file_path);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    const std::span<const char> file_view(mapping.data, mapping.size);

    try {
        corosio::io_context ctx;
        corosio::tcp_acceptor acceptor(ctx);

        acceptor.open(corosio::tcp::v4());
        acceptor.set_option(corosio::socket_option::reuse_address(true));

        auto bind_ec = acceptor.bind(corosio::endpoint(static_cast<std::uint16_t>(port)));
        if (bind_ec) {
            std::cerr << "bind: " << bind_ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        auto listen_ec = acceptor.listen(BACKLOG);
        if (listen_ec) {
            std::cerr << "listen: " << listen_ec.message() << "\n";
            unmap_file(mapping);
            return EXIT_FAILURE;
        }

        capy::run_async(ctx.get_executor())(
            accept_loop(ctx, acceptor, file_view)
        );

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back(run_io_context, std::ref(ctx));
        }

        for (auto& worker : pool) {
            worker.join();
        }

        unmap_file(mapping);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }
}