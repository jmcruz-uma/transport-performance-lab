cmake_minimum_required(VERSION 3.10)

project("BSD SOCKETS POLL" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -pedantic")

option(BSD_ENABLE_BENCHMARKS "Enable Google Benchmark benchmarks" ON)

# Every server/client here uses std::thread directly (a worker-thread pool,
# or none at all for the single-shot clients). On this project's target
# platform (Ubuntu 24.04, glibc >= 2.34) pthread is merged into libc, so
# std::thread already works without this -- confirmed empirically, not just
# assumed, 2026-09-19: a std::thread-spawning binary linked with no threading
# library at all still produced real OS threads in /proc/<pid>/task. Linked
# explicitly anyway for portability to any toolchain where that merge doesn't
# hold, and so `server_threads` -- a core factor of the experimental matrix --
# is never resting on an assumption about the deployment glibc instead of a
# declared dependency.
find_package(Threads REQUIRED)

# TLS scenarios (tls / tls_framed) need OpenSSL and the shared config header.
find_package(OpenSSL REQUIRED)
set(TLS_COMMON_DIR "${PROJECT_SOURCE_DIR}/../tls")

add_subdirectory(tcpserver)
add_subdirectory(tcpserver_framed)
add_subdirectory(tcpserver_tls)
add_subdirectory(tcpserver_tls_framed)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(BSD_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()