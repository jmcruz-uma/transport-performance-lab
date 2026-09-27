/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file client with BSD sockets
 * Manual smoke-test tool: downloads the served file and writes it to disk, to
 * check by hand that a server works. No campaign runs it; the measured clients
 * are in ../benchmarks/.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 65536;


static int connect_to_server(const std::string& server_ip, int port) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        return -1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<std::uint16_t>(port));

    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == -1) {
  
        close(sock);
        return -1;
    }

    return sock;
}


static bool receive_file(int sock, const std::string& output_path) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        return false;
    }

    std::array<char, BUFFER_SIZE> buffer;

    while (true) {
        const ssize_t n = recv(sock, buffer.data(), buffer.size(), 0);

        if (n > 0) {
            out.write(buffer.data(), static_cast<std::streamsize>(n));
            if (!out) {
                std::cerr << "Failed to write output file.\n";
                return false;
            }
            continue;
        }

        if (n == 0) {
            break;
        }

        std::cerr << "recv failed: " << std::strerror(errno) << "\n";
        return false;
    }

    out.close();
    return true;
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

    const int sock = connect_to_server(server_ip, port);
    if (sock == -1) {
        std::cerr << "connect failed\n";
        return EXIT_FAILURE;
    }

    const bool ok = receive_file(sock, output_path);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
