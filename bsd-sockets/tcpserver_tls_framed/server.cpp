/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with epoll() over TLS 1.3, length-prefixed framing
 * (scenario "tls_framed").
 *
 * Same non-blocking epoll + hand-written OpenSSL state machine as
 * ../tcpserver_tls/server.cpp, with framing: instead of one SSL_write cursor over
 * the whole payload, each client carries a cursor over the manifest
 * (tls/manifest.txt, path in TLS_MANIFEST) -- which message, how many header
 * bytes sent, how many body bytes sent -- and the worker drives SSL_write for the
 * 4-byte big-endian length then the body of every message, re-arming epoll for
 * whatever direction OpenSSL wants. This is what "BSD sockets + TLS + framing"
 * costs: the application owns the whole non-blocking loop and, since OpenSSL has
 * no gather write, spends two SSL_write calls per message (a 4-byte header and
 * the body); the wire bytes are byte-for-byte what taps_cpp's
 * LengthPrefixedFramer produces. Parameters come from ../../tls/tls_common.hpp.
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
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "tls_common.hpp"

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr std::size_t MAX_CLIENTS_PER_WORKER = 256;
constexpr int MAX_WORKER_THREADS = 256;
constexpr std::size_t LENGTH_PREFIX = 4;  // 4-byte big-endian, matches taps LengthPrefixedFramer

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

// One message size in bytes per line; '#' comments and blank lines ignored.
static std::vector<std::size_t> read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open manifest: " + path);
    std::vector<std::size_t> sizes;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') continue;
        sizes.push_back(static_cast<std::size_t>(std::stoull(line.substr(b))));
    }
    if (sizes.empty()) throw std::runtime_error("manifest has no message sizes: " + path);
    return sizes;
}

// Per-client cursor over the manifest.
struct ClientState {
    int fd = -1;
    SSL* ssl = nullptr;
    bool hs_done = false;
    std::size_t msg_idx = 0;     // next manifest entry to send
    std::size_t hdr_sent = 0;    // header bytes of msg_idx already written (0..LENGTH_PREFIX)
    std::size_t body_sent = 0;   // body bytes of msg_idx already written
    std::size_t body_off = 0;    // start offset of msg_idx's body in the payload
};

//   want_in / want_out -> re-arm epoll for that direction and wait
//   done               -> all manifest messages sent
//   error              -> drop the client
enum class TlsStep { want_in, want_out, done, error };

static TlsStep step_from_ssl_error(int e) {
    if (e == SSL_ERROR_WANT_READ) return TlsStep::want_in;
    if (e == SSL_ERROR_WANT_WRITE) return TlsStep::want_out;
    return TlsStep::error;
}

static TlsStep serve_tls_step(ClientState& cs, std::span<const char> payload,
                              const std::vector<std::size_t>& sizes) {
    if (!cs.hs_done) {
        const int r = SSL_accept(cs.ssl);
        if (r == 1) {
            cs.hs_done = true;
            static std::atomic<bool> printed{false};
            if (!printed.exchange(true)) tlscommon::print_tls_identity(cs.ssl, "server");
        } else {
            return step_from_ssl_error(SSL_get_error(cs.ssl, r));
        }
    }

    while (cs.msg_idx < sizes.size()) {
        std::size_t size = sizes[cs.msg_idx];
        if (size > payload.size()) size = payload.size();
        if (cs.hdr_sent == 0 && cs.body_sent == 0 && cs.body_off + size > payload.size()) {
            cs.body_off = 0;  // wrap, only decided at the start of a message
        }

        while (cs.hdr_sent < LENGTH_PREFIX) {
            const auto len = static_cast<std::uint32_t>(size);
            const unsigned char header[LENGTH_PREFIX] = {
                static_cast<unsigned char>(len >> 24),
                static_cast<unsigned char>(len >> 16),
                static_cast<unsigned char>(len >> 8),
                static_cast<unsigned char>(len)};
            const int r = SSL_write(cs.ssl, header + cs.hdr_sent,
                                    static_cast<int>(LENGTH_PREFIX - cs.hdr_sent));
            if (r > 0) { cs.hdr_sent += static_cast<std::size_t>(r); continue; }
            return step_from_ssl_error(SSL_get_error(cs.ssl, r));
        }

        while (cs.body_sent < size) {
            const std::size_t remaining = size - cs.body_sent;
            const int chunk = static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining);
            const int r = SSL_write(cs.ssl, payload.data() + cs.body_off + cs.body_sent, chunk);
            if (r > 0) { cs.body_sent += static_cast<std::size_t>(r); continue; }
            return step_from_ssl_error(SSL_get_error(cs.ssl, r));
        }

        cs.body_off += size;
        ++cs.msg_idx;
        cs.hdr_sent = 0;
        cs.body_sent = 0;
    }
    return TlsStep::done;
}

static std::size_t find_client_index(const std::array<ClientState, MAX_CLIENTS_PER_WORKER>& cs,
                                     std::size_t count, int fd) {
    for (std::size_t i = 0; i < count; ++i)
        if (cs[i].fd == fd) return i;
    return count;
}

static void arm(int epoll_fd, int fd, std::uint32_t direction) {
    epoll_event ev{};
    ev.events = direction | EPOLLERR | EPOLLHUP;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static void accept_loop(int listen_fd, SSL_CTX* ssl_ctx, std::span<const char> payload,
                        const std::vector<std::size_t>& sizes) {
    std::array<ClientState, MAX_CLIENTS_PER_WORKER> clients{};
    std::array<epoll_event, MAX_CLIENTS_PER_WORKER> events{};
    std::size_t client_count = 0;

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
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clients[index].fd, nullptr);
        if (clients[index].ssl) SSL_free(clients[index].ssl);
        close(clients[index].fd);
        const std::size_t last = client_count - 1;
        if (index != last) clients[index] = clients[last];
        clients[last] = ClientState{};
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
                        break;
                    }
                    if (!set_nonblocking(client_fd)) { close(client_fd); continue; }

                    SSL* ssl = SSL_new(ssl_ctx);
                    if (!ssl) { close(client_fd); continue; }
                    SSL_set_fd(ssl, client_fd);
                    SSL_set_accept_state(ssl);

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) == -1) {
                        SSL_free(ssl);
                        close(client_fd);
                        continue;
                    }
                    clients[client_count] = ClientState{};
                    clients[client_count].fd = client_fd;
                    clients[client_count].ssl = ssl;
                    ++client_count;
                }
                continue;
            }

            const std::size_t index = find_client_index(clients, client_count, fd);
            if (index == client_count) continue;

            if (revents & (EPOLLERR | EPOLLHUP)) { drop(index); continue; }

            switch (serve_tls_step(clients[index], payload, sizes)) {
                case TlsStep::want_in:  arm(epoll_fd, fd, EPOLLIN);  break;
                case TlsStep::want_out: arm(epoll_fd, fd, EPOLLOUT); break;
                case TlsStep::done:
                    SSL_shutdown(clients[index].ssl);  // best-effort close_notify
                    drop(index);
                    break;
                case TlsStep::error:
                    drop(index);
                    break;
            }
        }
    }

    for (std::size_t i = 0; i < client_count; ++i) {
        if (clients[i].ssl) SSL_free(clients[i].ssl);
        close(clients[i].fd);
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

    std::vector<std::size_t> sizes;
    try {
        sizes = read_manifest(tlscommon::env_or("TLS_MANIFEST", "../tls/manifest.txt"));
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
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
        workers[static_cast<std::size_t>(i)] =
            std::thread(accept_loop, listen_fd, ssl_ctx, payload, std::cref(sizes));
    for (int i = 0; i < threads; ++i)
        workers[static_cast<std::size_t>(i)].join();

    close(listen_fd);
    SSL_CTX_free(ssl_ctx);
    unmap_file(mapping);
    return EXIT_SUCCESS;
}
