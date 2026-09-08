find_package(Threads REQUIRED)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # The system's libbenchmark-dev is built against libstdc++; linking it
    # into a -stdlib=libc++ binary breaks on std::string/vector ABI, so
    # Clang builds its own copy instead (same version as the system package).
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3)
    FetchContent_MakeAvailable(googlebenchmark)
else()
    find_package(benchmark REQUIRED)
endif()

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
