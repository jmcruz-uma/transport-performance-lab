add_executable(bench_tcp bench_tcp.cpp)

target_link_libraries(bench_tcp
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp PRIVATE cxx_std_23)

add_executable(bench_tcp_whole bench_tcp_whole.cpp)

target_link_libraries(bench_tcp_whole
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp_whole PRIVATE cxx_std_23)

add_executable(bench_tcp_blocks bench_tcp_blocks.cpp)

target_link_libraries(bench_tcp_blocks
    PRIVATE
        TAPS::static
        Threads::Threads
        benchmark::benchmark
)

target_compile_features(bench_tcp_blocks PRIVATE cxx_std_23)

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
