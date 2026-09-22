/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with epoll(), length-prefixed framing, NO security layer
 * (scenario "framed" -- the plaintext mirror of "tls_framed", replacing the
 * old "blocks" scenario -- see ../../asio/tcpserver_framed/server.cpp for why).
 *
 * Same non-blocking epoll + send() loop as ../tcpserver/server.cpp, with
 * framing: instead of one send() cursor over the whole payload, each client
 * carries a cursor over the manifest (tls/manifest.txt, path in MANIFEST --
 * still under tls/: it's the same workload file "tls_framed" uses, reused
 * here so the two scenarios are directly comparable message-for-message) --
 * which message, how many header bytes sent, how many body bytes sent -- and
 * the worker drives send() for the 4-byte big-endian length then the body of
 * every message, re-arming epoll for EPOLLOUT whenever the kernel's send
 * buffer fills. This is what "BSD sockets + framing" costs on its own,
 * without TLS folded in: the application owns the whole non-blocking loop and
 * a plain socket has no gather-write either, so it spends two send() calls
 * per message (a 4-byte header and the body); the wire bytes are
 * byte-for-byte what taps_cpp's LengthPrefixedFramer produces.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
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

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr std::size_t MAX_CLIENTS_PER_WORKER = 256;
constexpr int MAX_WORKER_THREADS = 256;
constexpr std::size_t LENGTH_PREFIX = 4;  // 4-byte big-endian, matches taps LengthPrefixedFramer

static std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : fallback;
}

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
    std::size_t msg_idx = 0;     // next manifest entry to send
    std::size_t hdr_sent = 0;    // header bytes of msg_idx already written (0..LENGTH_PREFIX)
    std::size_t body_sent = 0;   // body bytes of msg_idx already written
    std::size_t body_off = 0;    // start offset of msg_idx's body in the payload
};

//   want_out -> re-arm epoll for EPOLLOUT and wait
//   done     -> all manifest messages sent
//   error    -> drop the client
enum class Step { want_out, done, error };

static Step serve_step(ClientState& cs, std::span<const char> payload,
                       const std::vector<std::size_t>& sizes) {
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
            const ssize_t n = send(cs.fd, header + cs.hdr_sent,
                                   LENGTH_PREFIX - cs.hdr_sent, MSG_NOSIGNAL);
            if (n > 0) { cs.hdr_sent += static_cast<std::size_t>(n); continue; }
            if (n == 0) return Step::error;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return Step::want_out;
            return Step::error;
        }

        while (cs.body_sent < size) {
            const std::size_t remaining = size - cs.body_sent;
            const ssize_t n = send(cs.fd, payload.data() + cs.body_off + cs.body_sent,
                                   remaining, MSG_NOSIGNAL);
            if (n > 0) { cs.body_sent += static_cast<std::size_t>(n); continue; }
            if (n == 0) return Step::error;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return Step::want_out;
            return Step::error;
        }

        cs.body_off += size;
        ++cs.msg_idx;
        cs.hdr_sent = 0;
        cs.body_sent = 0;
    }
    return Step::done;
}

static std::size_t find_client_index(const std::array<ClientState, MAX_CLIENTS_PER_WORKER>& cs,
                                     std::size_t count, int fd) {
    for (std::size_t i = 0; i < count; ++i)
        if (cs[i].fd == fd) return i;
    return count;
}

static void accept_loop(int listen_fd, std::span<const char> payload,
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

                    epoll_event cev{};
                    cev.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) == -1) {
                        close(client_fd);
                        continue;
                    }
                    clients[client_count] = ClientState{};
                    clients[client_count].fd = client_fd;
                    ++client_count;
                }
                continue;
            }

            const std::size_t index = find_client_index(clients, client_count, fd);
            if (index == client_count) continue;

            if (revents & (EPOLLERR | EPOLLHUP)) { drop(index); continue; }

            if (revents & EPOLLOUT) {
                switch (serve_step(clients[index], payload, sizes)) {
                    case Step::want_out: break;  // already armed for EPOLLOUT
                    case Step::done:
                    case Step::error:
                        drop(index);
                        break;
                }
            }
        }
    }

    for (std::size_t i = 0; i < client_count; ++i) close(clients[i].fd);
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
        sizes = read_manifest(env_or("MANIFEST", "../tls/manifest.txt"));
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

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { std::perror("socket"); unmap_file(mapping); return EXIT_FAILURE; }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::perror("setsockopt"); close(listen_fd); unmap_file(mapping); return EXIT_FAILURE;
    }
    if (!set_nonblocking(listen_fd)) {
        std::perror("fcntl"); close(listen_fd); unmap_file(mapping); return EXIT_FAILURE;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::perror("bind"); close(listen_fd); unmap_file(mapping); return EXIT_FAILURE;
    }
    if (listen(listen_fd, BACKLOG) == -1) {
        std::perror("listen"); close(listen_fd); unmap_file(mapping); return EXIT_FAILURE;
    }

    std::array<std::thread, MAX_WORKER_THREADS> workers{};
    for (int i = 0; i < threads; ++i)
        workers[static_cast<std::size_t>(i)] =
            std::thread(accept_loop, listen_fd, payload, std::cref(sizes));
    for (int i = 0; i < threads; ++i)
        workers[static_cast<std::size_t>(i)].join();

    close(listen_fd);
    unmap_file(mapping);
    return EXIT_SUCCESS;
}
