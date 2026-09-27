/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file client with async-berkeley
 * Minimal raw-byte client for performance and energy measurements.
 */

#include <io/io.hpp>

#include <arpa/inet.h>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

constexpr int DEFAULT_PORT = 8080;
constexpr std::size_t BUFFER_SIZE = 8192;

struct ClientState {
    dialog client;
    std::array<char, BUFFER_SIZE> buffer;
    std::ofstream output_file;
    bool failed = false;

    explicit ClientState(dialog&& socket)
        : client(std::move(socket)) {
    }
};

static constexpr auto error_handler = [](const auto& error) {
    if constexpr (std::is_same_v<std::decay_t<decltype(error)>, int>) {
        std::cerr << std::error_code(error, std::system_category()).message() << "\n";
    } else {
        std::cerr << "async operation failed\n";
    }
};

static void receive_file(
    async_scope& scope,
    std::shared_ptr<ClientState> state
) {
    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        state->buffer.data(),
        state->buffer.size()
    );

    auto operation =
        recvmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_received) {
            if (bytes_received <= 0) {
                state->output_file.close();
                return;
            }

            state->output_file.write(
                state->buffer.data(),
                static_cast<std::streamsize>(bytes_received)
            );

            if (!state->output_file) {
                std::cerr << "Failed to write output file.\n";
                state->failed = true;
                state->output_file.close();
                return;
            }

            receive_file(scope, state);
        })
        | upon_error([state, msg](const auto& error) {
            state->failed = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

static std::shared_ptr<ClientState> start_client(
    async_scope& scope,
    dialog&& client,
    socket_address<sockaddr_in> server_address,
    const std::string& output_path
) {
    auto state = std::make_shared<ClientState>(std::move(client));

    state->output_file.open(output_path, std::ios::binary);
    if (!state->output_file) {
        std::cerr << "Failed to open output file: " << output_path << "\n";
        state->failed = true;
        return state;
    }

    auto operation =
        io::connect(state->client, server_address)
        | then([&scope, state](const auto&) {
            receive_file(scope, state);
        })
        | upon_error([state](const auto& error) {
            state->failed = true;
            error_handler(error);
        });

    scope.spawn(std::move(operation));
    return state;
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

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

    async_scope scope;
    triggers trigs;

    auto client = trigs.emplace(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    auto server_address = make_address<sockaddr_in>();
    server_address->sin_family = AF_INET;
    server_address->sin_port = htons(static_cast<std::uint16_t>(port));
    server_address->sin_addr.s_addr = inet_addr(server_ip.c_str());

    auto state = start_client(
        scope,
        std::move(client),
        server_address,
        output_path
    );

    while (trigs.wait()) {
    }

    return state->failed ? EXIT_FAILURE : EXIT_SUCCESS;
}