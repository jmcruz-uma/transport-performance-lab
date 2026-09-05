cmake_minimum_required(VERSION 3.10)

project("ASIO STANDALONE" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -pedantic")

find_package(Threads REQUIRED)

# Standalone ASIO is fetched automatically (no manual setup needed) unless
# ASIO_INCLUDE_DIR already points at an existing checkout.
set(ASIO_INCLUDE_DIR "" CACHE PATH "Path to ASIO standalone headers (leave empty to fetch automatically)")
if(ASIO_INCLUDE_DIR STREQUAL "" OR NOT EXISTS "${ASIO_INCLUDE_DIR}/asio.hpp")
    include(FetchContent)
    FetchContent_Declare(asio_standalone
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-30-2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(asio_standalone)
    set(ASIO_INCLUDE_DIR "${asio_standalone_SOURCE_DIR}/asio/include" CACHE PATH
        "Path to ASIO standalone headers" FORCE)
endif()

option(ASIO_ENABLE_BENCHMARKS "Enable Google Benchmark benchmarks" ON)

add_subdirectory(tcpserver)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(ASIO_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()