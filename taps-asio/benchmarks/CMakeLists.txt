add_executable(bench_tcp bench_tcp.cpp)

target_link_libraries(bench_tcp
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp PRIVATE cxx_std_23)

add_executable(bench_tcp_whole_transfer bench_tcp_whole_transfer.cpp)

target_link_libraries(bench_tcp_whole_transfer
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp_whole_transfer PRIVATE cxx_std_23)

# --- naive variants (scenarios "streaming_naive", "whole_transfer_naive") ----
# Same sources as bench_tcp / bench_tcp_whole_transfer; the only difference is
# that message memory comes from the heap with no recycling (message_memory.hpp).
foreach(client bench_tcp bench_tcp_whole_transfer)
    add_executable(${client}_naive ${client}.cpp)
    target_compile_definitions(${client}_naive PRIVATE TAPS_NAIVE_MESSAGE_MEMORY)
    target_link_libraries(${client}_naive
        PRIVATE
            TAPS::static
            Threads::Threads
            benchmark::benchmark
    )
    target_compile_features(${client}_naive PRIVATE cxx_std_23)
endforeach()

# --- framed benchmark (scenario "framed", replaces old "blocks") -----------
add_executable(bench_tcp_framed bench_tcp_framed.cpp)

target_link_libraries(bench_tcp_framed
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp_framed PRIVATE cxx_std_23)

add_executable(bench_udp bench_udp.cpp)

target_link_libraries(bench_udp
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_udp PRIVATE cxx_std_23)

# --- TLS streaming benchmark (scenario "tls") ---------------------------------
add_executable(bench_tcp_tls bench_tcp_tls.cpp)

target_link_libraries(bench_tcp_tls
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_include_directories(bench_tcp_tls
    PRIVATE
        ${TLS_COMMON_DIR}
)

target_compile_features(bench_tcp_tls PRIVATE cxx_std_23)

# --- TLS framed benchmark (scenario "tls_framed") ----------------------------
add_executable(bench_tcp_tls_framed bench_tcp_tls_framed.cpp)

target_link_libraries(bench_tcp_tls_framed
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_include_directories(bench_tcp_tls_framed
    PRIVATE
        ${TLS_COMMON_DIR}
)

target_compile_features(bench_tcp_tls_framed PRIVATE cxx_std_23)

# --- "*_crc" variants (scenarios "<scenario>_crc"): the same clients, processing every
# received byte with the CRC-32C of ../../tls/consume_crc.hpp. Missing clients are skipped.
include(${TLS_COMMON_DIR}/consume_crc.cmake)
consume_crc_variants(bench_tcp bench_tcp_whole_transfer bench_tcp_naive bench_tcp_whole_transfer_naive
                     bench_tcp_framed bench_tcp_tls bench_tcp_tls_framed bench_udp)
