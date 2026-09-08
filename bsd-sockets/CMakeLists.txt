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

# TLS scenarios (tls / tls_framed) need OpenSSL and the shared config header.
find_package(OpenSSL REQUIRED)
set(TLS_COMMON_DIR "${PROJECT_SOURCE_DIR}/../tls")

add_subdirectory(tcpserver)
add_subdirectory(tcpserver_tls)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(BSD_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()