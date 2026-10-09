// Processing of received data for the "*_crc" scenarios, shared by every arm (and, like
// frame_reader.hpp, TLS-agnostic despite the directory).
//
// A client built with CONSUME_CRC runs a CRC-32C (Castagnoli, the SSE4.2 crc32
// instruction) over every byte of each complete unit it receives, as an application
// that checks what it gets would: the unit is whatever the client can consume as a
// whole (a delivered Message, after as_bytes() where the client calls it; a chunk read
// from the stream; a frame body; a datagram). One running CRC covers the whole
// download, so its value does not depend on how the bytes were split into units, and
// at the end it is compared with EXPECTED_CRC32C (hex) when that variable is set.
//
// Without CONSUME_CRC every function here compiles to nothing: the client discards
// the data, exactly as the scenarios without the "_crc" suffix measure it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <nmmintrin.h>

namespace consume {

class Crc32c {
public:
    [[gnu::target("sse4.2")]]
    void update(const std::byte* p, std::size_t n) noexcept {
        std::uint64_t c = state_;
        for (; n >= 8; p += 8, n -= 8) {
            std::uint64_t w;
            std::memcpy(&w, p, 8);  // unaligned load into a register, no buffer copy
            c = _mm_crc32_u64(c, w);
        }
        for (; n > 0; ++p, --n)
            c = _mm_crc32_u8(static_cast<std::uint32_t>(c), static_cast<std::uint8_t>(*p));
        state_ = static_cast<std::uint32_t>(c);
    }

    std::uint32_t value() const noexcept { return ~state_; }

private:
    std::uint32_t state_ = 0xFFFFFFFFu;
};

inline Crc32c g_crc;  // one download per client process

// Processes one complete unit of received bytes.
inline void bytes([[maybe_unused]] const void* p, [[maybe_unused]] std::size_t n) noexcept {
#ifdef CONSUME_CRC
    g_crc.update(static_cast<const std::byte*>(p), n);
#endif
}

// True when the download checks out: always without CONSUME_CRC or without
// EXPECTED_CRC32C (UDP may lose datagrams, so its scenarios do not set it).
inline bool verified() noexcept {
#ifdef CONSUME_CRC
    const char* expected = std::getenv("EXPECTED_CRC32C");
    return expected == nullptr ||
           g_crc.value() == static_cast<std::uint32_t>(std::strtoul(expected, nullptr, 16));
#else
    return true;
#endif
}

}  // namespace consume
