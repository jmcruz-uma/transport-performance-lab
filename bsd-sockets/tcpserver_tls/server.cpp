/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with epoll() over TLS 1.3.
 *
 * Identical to ../tcpserver/server.cpp (non-blocking epoll, mmap'd file, one
 * send step per EPOLLOUT) with ONE change: each accepted fd is driven through a
 * hand-written OpenSSL state machine -- SSL_accept during the handshake, then
 * SSL_write for the payload, re-arming epoll for whatever direction OpenSSL
 * wants next. This is what "BSD sockets + TLS" costs: the application owns the
 * whole non-blocking TLS loop. Parameters come from ../../tls/tls_common.hpp.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

#include "tls_common.hpp"

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr std::size_t MAX_CLIENTS_PER_WORKER = 256;
constexpr int MAX_WORKER_THREADS = 256;

struct FileMapping {
    int fd = -1;
    const char* data = nullptr;
    std::size_t size = 0;
};

static bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

static FileMapping map_file_read_only(const fs::path& path) {
    FileMapping mapping{};
    mapping.size = static_cast<std::size_t>(fs::file_size(path));
    mapping.fd = open(path.c_str(), O_RDONLY);
    if (mapping.fd == -1) throw std::runtime_error("Failed to open file: " + path.string());
    if (mapping.size == 0) return mapping;
    void* ptr = mmap(nullptr, mapping.size, PROT_READ, MAP_PRIVATE, mapping.fd, 0);
    if (ptr == MAP_FAILED) { close(mapping.fd); throw std::runtime_error("mmap failed."); }
    mapping.data = static_cast<const char*>(ptr);
    return mapping;
}

static void unmap_file(FileMapping& mapping) {
    if (mapping.data != nullptr && mapping.size > 0)
        munmap(const_cast<char*>(mapping.data), mapping.size);
    if (mapping.fd != -1) close(mapping.fd);
    mapping = FileMapping{};
}

// One non-blocking step for a client: run the handshake, then push the payload.
//   want_in / want_out -> re-arm epoll for that direction and wait
//   done               -> payload fully sent
//   error              -> drop the client
enum class TlsStep { want_in, want_out, done, error };

static TlsStep serve_tls_step(SSL* ssl, bool& hs_done, std::size_t& sent,
                              std::span<const char> payload) {
    if (!hs_done) {
        const int r = SSL_accept(ssl);
        if (r == 1) {
            hs_done = true;
            static std::atomic<bool> printed{false};
            if (!printed.exchange(true)) tlscommon::print_tls_identity(ssl, "server");
        } else {
            const int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ) return TlsStep::want_in;
            if (e == SSL_ERROR_WANT_WRITE) return TlsStep::want_out;
            return TlsStep::error;
        }
    }

    while (sent < payload.size()) {
        const int r = SSL_write(ssl, payload.data() + sent,
                                static_cast<int>(payload.size() - sent > INT_MAX
                                                     ? INT_MAX : payload.size() - sent));
        if (r > 0) {
            sent += static_cast<std::size_t>(r);
            continue;
        }
        const int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ) return TlsStep::want_in;
        if (e == SSL_ERROR_WANT_WRITE) return TlsStep::want_out;
        return TlsStep::error;
    }
    return TlsStep::done;
}

static std::size_t find_client_index(const std::array<int, MAX_CLIENTS_PER_WORKER>& fds,
                                     std::size_t count, int fd) {
    for (std::size_t i = 0; i < count; ++i)
        if (fds[i] == fd) return i;
    return count;
}

static void arm(int epoll_fd, int fd, std::uint32_t direction) {
    epoll_event ev{};
    ev.events = direction | EPOLLERR | EPOLLHUP;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static void accept_loop(int listen_fd, SSL_CTX* ssl_ctx, std::span<const char> payload) {
    std::array<int, MAX_CLIENTS_PER_WORKER> client_fds{};
    std::array<SSL*, MAX_CLIENTS_PER_WORKER> client_ssl{};
    std::array<bool, MAX_CLIENTS_PER_WORKER> client_hs{};
    std::array<std::size_t, MAX_CLIENTS_PER_WORKER> client_sent{};
    std::array<epoll_event, MAX_CLIENTS_PER_WORKER> events{};

    std::size_t client_count = 0;
    client_fds.fill(-1);
    client_ssl.fill(nullptr);
    client_hs.fill(false);
    client_sent.fill(0);

    const int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed: " << std::strerror(errno) << "\n";
        return;
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event) == -1) {
        std::cerr << "epoll_ctl listen_fd failed: " << std::strerror(errno) << "\n";
        close(epoll_fd);
        return;
    }

    auto drop = [&](std::size_t index) {
        const int fd = client_fds[index];
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        if (client_ssl[index]) SSL_free(client_ssl[index]);
        close(fd);
        const std::size_t last = client_count - 1;
        if (index != last) {
            client_fds[index] = client_fds[last];
            client_ssl[index] = client_ssl[last];
            client_hs[index] = client_hs[last];
            client_sent[index] = client_sent[last];
        }
        client_fds[last] = -1;
        client_ssl[last] = nullptr;
        client_hs[last] = false;
        client_sent[last] = 0;
        --client_count;
    };

    while (true) {
        const int ready = epoll_wait(epoll_fd, events.data(),
                                     static_cast<int>(events.size()), -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << "\n";
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t revents = events[static_cast<std::size_t>(i)].events;

            if (fd == listen_fd) {
                while (client_count < MAX_CLIENTS_PER_WORKER) {
                    const int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd == -1) {
                        if (errno == EINTR) continue;
                        break;  // EAGAIN / EWOULDBLOCK / other
                    }
                    if (!set_nonblocking(client_fd)) { close(client_fd); continue; }

                    SSL* ssl = SSL_new(ssl_ctx);
                    if (!ssl) { close(client_fd); continue; }
                    SSL_set_fd(ssl, client_fd);
                    SSL_set_accept_state(ssl);

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLERR | EPOLLHUP;  // handshake reads first
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) == -1) {
                        SSL_free(ssl);
                        close(client_fd);
                        continue;
                    }
                    client_fds[client_count] = client_fd;
                    client_ssl[client_count] = ssl;
                    client_hs[client_count] = false;
                    client_sent[client_count] = 0;
                    ++client_count;
                }
                continue;
            }

            const std::size_t index = find_client_index(client_fds, client_count, fd);
            if (index == client_count) continue;

            if (revents & (EPOLLERR | EPOLLHUP)) { drop(index); continue; }

            switch (serve_tls_step(client_ssl[index], client_hs[index],
                                   client_sent[index], payload)) {
                case TlsStep::want_in:  arm(epoll_fd, fd, EPOLLIN);  break;
                case TlsStep::want_out: arm(epoll_fd, fd, EPOLLOUT); break;
                case TlsStep::done:
                    SSL_shutdown(client_ssl[index]);  // best-effort close_notify
                    drop(index);
                    break;
                case TlsStep::error:
                    drop(index);
                    break;
            }
        }
    }

    for (std::size_t i = 0; i < client_count; ++i) {
        if (client_ssl[i]) SSL_free(client_ssl[i]);
        close(client_fds[i]);
    }
    close(epoll_fd);
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
        if (port <= 0 || port > 65535) { std::cerr << "Invalid port.\n"; return EXIT_FAILURE; }
    }
    if (argc == 4) {
        threads = std::stoi(argv[3]);
        if (threads <= 0 || threads > MAX_WORKER_THREADS) {
            std::cerr << "Invalid thread count.\n"; return EXIT_FAILURE;
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
    const std::span<const char> payload(mapping.data, mapping.size);

    SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx ||
        !tlscommon::configure_server_ctx(ssl_ctx,
                                         tlscommon::env_or("TLS_CERT", "../tls/server.crt"),
                                         tlscommon::env_or("TLS_KEY", "../tls/server.key"))) {
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
        unmap_file(mapping);
        return EXIT_FAILURE;
    }
    // Progressive partial writes: SSL_write may advance `sent` in chunks, and the
    // payload pointer moves between calls.
    SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                  SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { std::perror("socket"); SSL_CTX_free(ssl_ctx); unmap_file(mapping); return EXIT_FAILURE; }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::perror("setsockopt"); close(listen_fd); SSL_CTX_free(ssl_ctx); unmap_file(mapping); return EXIT_FAILURE;
    }
    if (!set_nonblocking(listen_fd)) {
        std::perror("fcntl"); close(listen_fd); SSL_CTX_free(ssl_ctx); unmap_file(mapping); return EXIT_FAILURE;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::perror("bind"); close(listen_fd); SSL_CTX_free(ssl_ctx); unmap_file(mapping); return EXIT_FAILURE;
    }
    if (listen(listen_fd, BACKLOG) == -1) {
        std::perror("listen"); close(listen_fd); SSL_CTX_free(ssl_ctx); unmap_file(mapping); return EXIT_FAILURE;
    }

    std::array<std::thread, MAX_WORKER_THREADS> workers{};
    for (int i = 0; i < threads; ++i)
        workers[static_cast<std::size_t>(i)] = std::thread(accept_loop, listen_fd, ssl_ctx, payload);
    for (int i = 0; i < threads; ++i)
        workers[static_cast<std::size_t>(i)].join();

    close(listen_fd);
    SSL_CTX_free(ssl_ctx);
    unmap_file(mapping);
    return EXIT_SUCCESS;
}
