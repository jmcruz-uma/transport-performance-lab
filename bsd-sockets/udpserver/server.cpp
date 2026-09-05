/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * BSD sockets UDP file server (scenarios "udp_k64"/"udp_k1400", E4).
 * One socket, no per-client state: on each request datagram the whole file is
 * streamed back to the requester's address as fixed-size datagrams (DGRAM_BYTES,
 * default the max IPv4 UDP payload, 65507 bytes), followed by a zero-length
 * datagram -- the end-of-transfer sentinel, since UDP has no end-of-stream of
 * its own. `threads` workers share one socket: recvfrom() from multiple
 * threads on the same fd is well-defined on Linux, each arriving datagram
 * going to exactly one waiting thread.
 *
 * Deliberately uses the OS's default socket buffer sizes, like every other
 * scenario: any loss it causes at these datagram sizes is a real, comparable
 * measurement, not an artifact of tuning some implementations and not others.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_THREADS = 256;
// 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
// payload a single sendto() can carry; anything above it fails.
constexpr std::size_t DEFAULT_DGRAM_BYTES = 65507;

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

static std::size_t dgram_bytes() {
    if (const char* s = std::getenv("DGRAM_BYTES")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v > 0) {
            return static_cast<std::size_t>(v);
        }
    }
    return DEFAULT_DGRAM_BYTES;
}

static void serve_requester(int sock, const sockaddr_in& client, std::span<const char> payload,
                            std::size_t dgram_size) {
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const std::size_t n = std::min(dgram_size, payload.size() - sent);
        const ssize_t rc = sendto(sock, payload.data() + sent, n, 0,
                                  reinterpret_cast<const sockaddr*>(&client), sizeof(client));
        if (rc <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(rc);
    }
    // Zero-length datagram: end-of-transfer sentinel.
    sendto(sock, nullptr, 0, 0, reinterpret_cast<const sockaddr*>(&client), sizeof(client));
}

static void serve_loop(int sock, std::span<const char> payload, std::size_t dgram_size) {
    char request[1];
    for (;;) {
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);
        const ssize_t n = recvfrom(sock, request, sizeof(request), 0,
                                   reinterpret_cast<sockaddr*>(&client), &client_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        serve_requester(sock, client, payload, dgram_size);
    }
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

    FileMapping mapping{};
    try {
        mapping = map_file_read_only(file_path);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    const std::span<const char> payload(mapping.data, mapping.size);
    const std::size_t dgram_size = dgram_bytes();

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        std::cerr << "socket failed\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(static_cast<std::uint16_t>(port));

    if (bind(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
        std::cerr << "bind failed\n";
        close(sock);
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back(serve_loop, sock, payload, dgram_size);
    }
    for (auto& worker : pool) {
        worker.join();
    }

    close(sock);
    unmap_file(mapping);
    return EXIT_SUCCESS;
}
