cmake_minimum_required(VERSION 3.14)

project("COROSIO" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler: ${CMAKE_CXX_COMPILER}")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -pedantic")

option(COROSIO_ENABLE_BENCHMARKS "Enable Google Benchmark benchmarks" ON)

include(FetchContent)

FetchContent_Declare(capy
    GIT_REPOSITORY https://github.com/cppalliance/capy.git
    GIT_TAG develop
    GIT_SHALLOW TRUE
)

FetchContent_Declare(corosio
    GIT_REPOSITORY https://github.com/cppalliance/corosio.git
    GIT_TAG develop
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(capy corosio)

add_subdirectory(tcpserver)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(COROSIO_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()