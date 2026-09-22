/*
 * Copyright (c) 2026 Jose Antonio Garcia Montanez
 *
 * TCP file server with async-berkeley, length-prefixed framing, NO security
 * layer (scenario "framed" -- the plaintext mirror of "tls_framed" the other
 * four arms have, replacing the old "blocks" scenario -- see
 * ../../asio/tcpserver_framed/server.cpp for the full rationale: taps_cpp's
 * PassthroughFramer can only ever emit after the connection's half-close,
 * with no boundary marker to key off on a raw-until-close wire, so it was
 * structurally incapable of the incremental delivery "blocks" wanted to
 * measure. LengthPrefixedFramer already does real incremental delivery, and
 * "tls_framed" already proves it end to end -- this scenario is that same
 * design, applied here for the first time to async-berkeley, which has no
 * TLS arm to mirror at all (excluded from tls/tls_framed: no security
 * integration point without hand-written glue equivalent to what BSD sockets
 * already contributes). Written fresh in this library's own idiom -- a
 * sender/receiver continuation per I/O step, exactly the shape
 * ../tcpserver/server.cpp already uses for a single unframed send -- against
 * the wire protocol the other four arms already validated, not against any
 * async-berkeley-specific precedent, since none exists.
 *
 * Sends N discrete messages whose sizes come from the shared manifest
 * (tls/manifest.txt, path in MANIFEST -- still under tls/: it's the same
 * workload file "tls_framed" uses, reused here so every "framed" arm is
 * directly comparable message-for-message; nothing about the manifest itself
 * is TLS-specific). Each message goes out as two sequential, individually
 * fully-retried phases: a 4-byte big-endian length header, then the body --
 * the same two-call shape bsd-sockets' framed server uses (plain sockets, no
 * gather-write primitive assumed here either, for symmetry across the
 * non-gather arms), rather than a single scatter/gather write whose partial
 * completion could split mid-header. Message bodies are slices of the
 * mmap'd payload, walked in order and wrapped around, so the wire bytes stay
 * deterministic; this is byte-for-byte the frame format taps_cpp's
 * LengthPrefixedFramer produces.
 */

#include <io/io.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace io;
using namespace io::socket;
using namespace io::execution;
using namespace stdexec;
using namespace exec;

using triggers = basic_triggers<poll_multiplexer>;
using dialog = socket_dialog<poll_multiplexer>;
using message = socket_message<sockaddr_in>;

namespace fs = std::filesystem;

constexpr int DEFAULT_PORT = 8080;
constexpr int BACKLOG = 128;
constexpr int MAX_THREADS = 256;
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
        ::close(mapping.fd);
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
        ::close(mapping.fd);
    }

    mapping.data = nullptr;
    mapping.fd = -1;
    mapping.size = 0;
}

// One message size in bytes per line; '#' comments and blank lines ignored.
static std::vector<std::size_t> read_manifest(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open manifest: " + path);
    }
    std::vector<std::size_t> sizes;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos || line[b] == '#') {
            continue;
        }
        sizes.push_back(static_cast<std::size_t>(std::stoull(line.substr(b))));
    }
    if (sizes.empty()) {
        throw std::runtime_error("manifest has no message sizes: " + path);
    }
    return sizes;
}

static constexpr auto error_handler = [](const auto& error) {
    if constexpr (std::is_same_v<std::decay_t<decltype(error)>, int>) {
        std::cerr << std::error_code(error, std::system_category()).message() << "\n";
    } else {
        std::cerr << "async operation failed\n";
    }
};

// Per-client cursor over the manifest: which message, and how far into its
// header/body phase the client currently is.
struct WriteState {
    dialog client;
    std::span<const char> payload;
    std::span<const std::size_t> sizes;

    std::size_t msg_idx = 0;       // next manifest entry to send
    std::size_t body_off = 0;      // start offset of msg_idx's body in the payload
    std::size_t current_size = 0;  // msg_idx's clamped body size
    std::array<unsigned char, LENGTH_PREFIX> header{};
    std::size_t header_sent = 0;
    std::size_t body_sent = 0;

    WriteState(dialog&& accepted_client, std::span<const char> file_payload,
              std::span<const std::size_t> manifest_sizes)
        : client(std::move(accepted_client)),
          payload(file_payload),
          sizes(manifest_sizes) {
    }
};

static void advance_to_next_message(async_scope& scope, std::shared_ptr<WriteState> state);
static void send_header(async_scope& scope, std::shared_ptr<WriteState> state);
static void send_body(async_scope& scope, std::shared_ptr<WriteState> state);

static void advance_to_next_message(async_scope& scope, std::shared_ptr<WriteState> state) {
    if (state->msg_idx >= state->sizes.size()) {
        return;  // every manifest message sent
    }

    std::size_t size = state->sizes[state->msg_idx];
    if (size > state->payload.size()) {
        size = state->payload.size();
    }
    if (state->body_off + size > state->payload.size()) {
        state->body_off = 0;  // wrap, only decided at the start of a message
    }
    state->current_size = size;

    const auto len = static_cast<std::uint32_t>(size);
    state->header[0] = static_cast<unsigned char>(len >> 24);
    state->header[1] = static_cast<unsigned char>(len >> 16);
    state->header[2] = static_cast<unsigned char>(len >> 8);
    state->header[3] = static_cast<unsigned char>(len);
    state->header_sent = 0;

    send_header(scope, state);
}

static void send_header(async_scope& scope, std::shared_ptr<WriteState> state) {
    if (state->header_sent >= state->header.size()) {
        state->body_sent = 0;
        send_body(scope, state);
        return;
    }

    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        reinterpret_cast<char*>(state->header.data() + state->header_sent),
        state->header.size() - state->header_sent
    );

    auto operation =
        sendmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_sent) {
            if (bytes_sent <= 0) {
                return;
            }
            state->header_sent += static_cast<std::size_t>(bytes_sent);
            send_header(scope, state);
        })
        | upon_error([state, msg](const auto& error) {
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

static void send_body(async_scope& scope, std::shared_ptr<WriteState> state) {
    if (state->body_sent >= state->current_size) {
        ++state->msg_idx;
        state->body_off += state->current_size;
        advance_to_next_message(scope, state);
        return;
    }

    auto msg = std::make_shared<message>();
    msg->buffers.emplace_back(
        const_cast<char*>(state->payload.data() + state->body_off + state->body_sent),
        state->current_size - state->body_sent
    );

    auto operation =
        sendmsg(state->client, *msg, 0)
        | then([&scope, state, msg](ssize_t bytes_sent) {
            if (bytes_sent <= 0) {
                return;
            }
            state->body_sent += static_cast<std::size_t>(bytes_sent);
            send_body(scope, state);
        })
        | upon_error([state, msg](const auto& error) {
            error_handler(error);
        });

    scope.spawn(std::move(operation));
}

static void serve_client(
    async_scope& scope,
    dialog&& client,
    std::span<const char> payload,
    std::span<const std::size_t> sizes
) {
    auto state = std::make_shared<WriteState>(
        std::move(client),
        payload,
        sizes
    );

    advance_to_next_message(scope, state);
}

static void accept_loop(
    async_scope& scope,
    const dialog& server,
    std::span<const char> payload,
    std::span<const std::size_t> sizes
) {
    auto operation =
        accept(server)
        | then([&scope, &server, payload, sizes](auto result) {
            auto [client, addr] = std::move(result);
            (void)addr;

            serve_client(scope, std::move(client), payload, sizes);
            accept_loop(scope, server, payload, sizes);
        })
        | upon_error(error_handler);

    scope.spawn(std::move(operation));
}

static void run_triggers(triggers& trigs) {
    while (trigs.wait()) {
    }
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

    try {
        async_scope scope;
        triggers trigs;

        auto server = trigs.emplace(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        auto server_address = make_address<sockaddr_in>();
        server_address->sin_family = AF_INET;
        server_address->sin_addr.s_addr = htonl(INADDR_ANY);
        server_address->sin_port = htons(static_cast<std::uint16_t>(port));

        socket_option<int> reuse{1};
        if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reuse)) {
            throw std::system_error(
                {errno, std::system_category()},
                "setsockopt failed"
            );
        }

        if (::io::bind(server, server_address)) {
            throw std::system_error(
                {errno, std::system_category()},
                "bind failed"
            );
        }

        if (::io::listen(server, BACKLOG)) {
            throw std::system_error(
                {errno, std::system_category()},
                "listen failed"
            );
        }

        accept_loop(scope, server, payload, std::span<const std::size_t>(sizes));

        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));

        for (int i = 0; i < threads; ++i) {
            pool.emplace_back(run_triggers, std::ref(trigs));
        }

        for (auto& worker : pool) {
            worker.join();
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unmap_file(mapping);
        return EXIT_FAILURE;
    }

    unmap_file(mapping);
    return EXIT_SUCCESS;
}
