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

target_link_libraries(bench_tcp
    PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    Boost::corosio
    pthread
)

target_compile_features(bench_tcp PRIVATE cxx_std_23)

add_executable(bench_tcp_whole bench_tcp_whole.cpp)

target_link_libraries(bench_tcp_whole
    PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    Boost::corosio
    pthread
)

target_compile_features(bench_tcp_whole PRIVATE cxx_std_23)

add_executable(bench_tcp_blocks bench_tcp_blocks.cpp)

target_link_libraries(bench_tcp_blocks
    PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    Boost::corosio
    pthread
)

target_compile_features(bench_tcp_blocks PRIVATE cxx_std_23)

add_executable(bench_udp bench_udp.cpp)

target_link_libraries(bench_udp
    PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    Boost::corosio
    pthread
)

target_compile_features(bench_udp PRIVATE cxx_std_23)

# --- TLS streaming benchmark (scenario "tls") ---------------------------------
add_executable(bench_tcp_tls bench_tcp_tls.cpp)

target_include_directories(bench_tcp_tls PRIVATE ${TLS_COMMON_DIR})

target_link_libraries(bench_tcp_tls
    PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    Boost::corosio_openssl   # OpenSSL TLS backend; pulls Boost::corosio + OpenSSL
    pthread
    OpenSSL::Crypto          # for OpenSSL_version() called directly
)

target_compile_features(bench_tcp_tls PRIVATE cxx_std_23)
