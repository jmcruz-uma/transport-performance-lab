// Message memory for the TAPS clients.
//
// Each client source builds two binaries. The default one (bench_tcp,
// bench_tcp_whole_transfer) leaves the configuration at its default: the
// library's pool, which recycles receive blocks across Messages. The naive one
// (bench_tcp_naive, bench_tcp_whole_transfer_naive: same source, compiled with
// TAPS_NAIVE_MESSAGE_MEMORY) takes message memory straight from the heap with no
// recycling: every receive block is a new allocation, freed when the Messages
// that use it are released. Nothing else differs between the two.

#pragma once

#include "taps/taps_api.h"

#include <memory_resource>

inline taps::MessageMemoryConfig client_message_memory() {
#ifdef TAPS_NAIVE_MESSAGE_MEMORY
    return taps::MessageMemoryConfig{std::pmr::new_delete_resource()};
#else
    return {};
#endif
}
