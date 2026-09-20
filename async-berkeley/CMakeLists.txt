cmake_minimum_required(VERSION 3.16)

project(async_file_transfer
    VERSION 1.0.0
    DESCRIPTION "TCP file transfer and benchmarking with async-berkeley"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")

option(ASYNC_TCP_ENABLE_BENCHMARKS "Enable Google Benchmark benchmarks" ON)

# Fetched from GitHub by default (no manual setup needed, pinned to the commit
# this branch was validated against); set ASYNC_BERKELEY_DIR yourself to reuse
# an existing checkout instead.
set(ASYNC_BERKELEY_DIR "" CACHE PATH
    "Path to an existing async-berkeley checkout (leave empty to fetch automatically)")

set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -w")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -w")

if(ASYNC_BERKELEY_DIR STREQUAL "" OR NOT EXISTS "${ASYNC_BERKELEY_DIR}/CMakeLists.txt")
    include(FetchContent)
    FetchContent_Declare(async_berkeley
        GIT_REPOSITORY https://github.com/kcexn/async-berkeley.git
        GIT_TAG dfc58059ec754a5de774173e2a4837778f6dc996)
    FetchContent_MakeAvailable(async_berkeley)
else()
    add_subdirectory("${ASYNC_BERKELEY_DIR}" "${CMAKE_BINARY_DIR}/async-berkeley-build")
endif()

# "framed" reuses tls/frame_reader.hpp (TLS-agnostic despite the directory --
# it's the shared deframing loop every "framed"/"tls_framed" client uses).
set(TLS_COMMON_DIR "${PROJECT_SOURCE_DIR}/../tls")

add_subdirectory(tcpserver)
add_subdirectory(tcpserver_framed)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(ASYNC_TCP_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()