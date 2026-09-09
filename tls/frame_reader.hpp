// Length-prefixed frame reader shared by the non-TAPS `tls_framed` clients
// (asio, bsd-sockets, capy-corosio). Header-only, no dependencies.
//
// Every one of those clients has to implement application framing by hand -- the
// competent way, which is the same in any technology: read as much as the
// transport gives you into a buffer, parse out every complete frame, keep the
// partial remainder, read more. Reading "exactly 4 bytes then exactly N" is the
// naive version and is not what a real program does. This class is that competent
// loop, factored out so the three arms are byte-for-byte identical in read size,
// number of reads and (zero) body copies -- the TAPS arm's receive_with_framing()
// does the same thing internally over its block chain.
//
// Frame format: a 4-byte big-endian unsigned length, then that many body bytes.
// Identical to taps::LengthPrefixedFramer{} (its default: 4 bytes, big-endian).
//
// Buffer policy: capacity starts at kReadChunk (64 KiB, the fixed "application
// receive buffer" comparability parameter) and grows only to hold a single frame
// larger than that -- mirroring the TAPS arm, whose block chain also grows for a
// large Message. Each read is still capped at kReadChunk, so the read count per
// byte transferred matches across arms regardless of frame size.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace tlsframe {

inline constexpr std::size_t kLengthPrefix = 4;
inline constexpr std::size_t kReadChunk = 64 * 1024;
// Sanity bound on a single frame; a header claiming more is treated as corrupt
// (the frame never completes, so the client's aggregate check fails and says so).
inline constexpr std::size_t kMaxFrame = 64 * 1024 * 1024;

class FrameReader {
public:
    FrameReader() : buf_(kReadChunk) {}

    // Writable region for the next read; never larger than kReadChunk. Call
    // committed() with the number of bytes actually read into it.
    std::span<char> read_span() {
        compact();
        // If we already know the in-flight frame's length, make sure the buffer
        // is big enough to hold it whole before reading more.
        if (used_ >= kLengthPrefix) {
            const std::size_t len = peek_length();
            const std::size_t need = kLengthPrefix + len;
            if (len <= kMaxFrame && need > buf_.size()) buf_.resize(need);
        }
        if (buf_.size() - used_ < kReadChunk && used_ == buf_.size()) {
            buf_.resize(buf_.size() + kReadChunk);
        }
        const std::size_t room = std::min(kReadChunk, buf_.size() - used_);
        return {buf_.data() + used_, room};
    }

    void committed(std::size_t n) { used_ += n; }

    // Next complete frame body as a view into the buffer, valid until the next
    // read_span(). std::nullopt means "need more bytes".
    std::optional<std::span<const char>> next_frame() {
        if (used_ - parsed_ < kLengthPrefix) return std::nullopt;
        const std::size_t len = peek_length();
        if (used_ - parsed_ < kLengthPrefix + len) return std::nullopt;
        const char* body = buf_.data() + parsed_ + kLengthPrefix;
        parsed_ += kLengthPrefix + len;
        return std::span<const char>{body, len};
    }

    // True if bytes remain that do not form a complete frame -- a truncated
    // stream when seen at EOF.
    bool has_partial() const { return parsed_ != used_; }

private:
    std::size_t peek_length() const {
        const auto* p = reinterpret_cast<const unsigned char*>(buf_.data() + parsed_);
        return (static_cast<std::size_t>(p[0]) << 24) |
               (static_cast<std::size_t>(p[1]) << 16) |
               (static_cast<std::size_t>(p[2]) << 8) |
               static_cast<std::size_t>(p[3]);
    }

    void compact() {
        if (parsed_ == 0) return;
        const std::size_t rem = used_ - parsed_;
        if (rem > 0) std::memmove(buf_.data(), buf_.data() + parsed_, rem);
        used_ = rem;
        parsed_ = 0;
    }

    std::vector<char> buf_;
    std::size_t used_ = 0;    // valid bytes in buf_
    std::size_t parsed_ = 0;  // offset of first unparsed byte
};

}  // namespace tlsframe
