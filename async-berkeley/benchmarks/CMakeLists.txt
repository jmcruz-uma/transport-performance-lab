find_package(benchmark REQUIRED)

add_executable(bench_tcp
    bench_tcp.cpp
)

target_link_libraries(bench_tcp
    PRIVATE
        AsyncBerk::asyncberk
        benchmark::benchmark
)

target_compile_features(bench_tcp
    PRIVATE
        cxx_std_23
)

add_executable(bench_tcp_whole
    bench_tcp_whole.cpp
)

target_link_libraries(bench_tcp_whole
    PRIVATE
        AsyncBerk::asyncberk
        benchmark::benchmark
)

target_compile_features(bench_tcp_whole
    PRIVATE
        cxx_std_23
)

add_executable(bench_tcp_blocks
    bench_tcp_blocks.cpp
)

target_link_libraries(bench_tcp_blocks
    PRIVATE
        AsyncBerk::asyncberk
        benchmark::benchmark
)

target_compile_features(bench_tcp_blocks
    PRIVATE
        cxx_std_23
)

add_executable(bench_udp
    bench_udp.cpp
)

target_link_libraries(bench_udp
    PRIVATE
        AsyncBerk::asyncberk
        benchmark::benchmark
)

target_compile_features(bench_udp
    PRIVATE
        cxx_std_23
)
