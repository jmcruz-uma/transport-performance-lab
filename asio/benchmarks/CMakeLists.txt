find_package(Threads REQUIRED)
find_package(benchmark REQUIRED)

add_executable(bench_tcp bench_tcp.cpp)

target_compile_features(bench_tcp PRIVATE cxx_std_23)

target_compile_definitions(bench_tcp
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(bench_tcp
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(bench_tcp
    PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
)

add_executable(bench_tcp_whole bench_tcp_whole.cpp)

target_compile_features(bench_tcp_whole PRIVATE cxx_std_23)

target_compile_definitions(bench_tcp_whole
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(bench_tcp_whole
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(bench_tcp_whole
    PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
)

add_executable(bench_tcp_blocks bench_tcp_blocks.cpp)

target_compile_features(bench_tcp_blocks PRIVATE cxx_std_23)

target_compile_definitions(bench_tcp_blocks
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(bench_tcp_blocks
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(bench_tcp_blocks
    PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
)

add_executable(bench_udp bench_udp.cpp)

target_compile_features(bench_udp PRIVATE cxx_std_23)

target_compile_definitions(bench_udp
    PRIVATE
        ASIO_STANDALONE
)

target_include_directories(bench_udp
    PRIVATE
        ${ASIO_INCLUDE_DIR}
)

target_link_libraries(bench_udp
    PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
)
