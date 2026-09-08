cmake_minimum_required(VERSION 3.20)

project(taps_asio_tcp LANGUAGES CXX)

option(TAPS_ENABLE_BENCHMARKS "Build benchmarks" ON)
option(TAPS_TCP_USE_LIBCXX "Use libc++ with Clang" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(TAPS_TCP_USE_LIBCXX)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

# taps_cpp is fetched from GitHub by default (no manual build/install needed,
# and it self-fetches its own ASIO dependency in turn); set TAPS_ROOT yourself
# to reuse an existing install (an <install>/include + <install>/lib/libtaps.a
# tree, e.g. from `cmake --install`) instead.
if(DEFINED TAPS_ROOT)
    set(TAPS_INCLUDE_DIR "${TAPS_ROOT}/include")
    set(TAPS_LIBRARY "${TAPS_ROOT}/lib/libtaps.a")

    if(NOT EXISTS "${TAPS_INCLUDE_DIR}/taps/taps_api.h")
        message(FATAL_ERROR "TAPS header not found: ${TAPS_INCLUDE_DIR}/taps/taps_api.h")
    endif()
    if(NOT EXISTS "${TAPS_LIBRARY}")
        message(FATAL_ERROR "TAPS library not found: ${TAPS_LIBRARY}")
    endif()

    message(STATUS "TAPS_ROOT: ${TAPS_ROOT}")
    message(STATUS "TAPS_INCLUDE_DIR: ${TAPS_INCLUDE_DIR}")
    message(STATUS "TAPS_LIBRARY: ${TAPS_LIBRARY}")

    add_library(TAPS::static STATIC IMPORTED GLOBAL)
    set_target_properties(TAPS::static PROPERTIES
        IMPORTED_LOCATION "${TAPS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TAPS_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE
    )
else()
    set(TAPS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TAPS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    # Track the tip of taps_cpp's default branch, NOT a pinned commit. Every
    # feature that lands on taps_cpp main exists for these experiments and is
    # validated there before being pushed; a frozen SHA here only creates a
    # second place to get the version wrong (the bug we hit bumping 84e6522).
    # build_release.sh wipes the build dir each run, so _deps/ is re-populated
    # and this re-clones the current main every time. Do not re-pin this.
    FetchContent_Declare(taps_cpp
        GIT_REPOSITORY https://github.com/jmcruz-uma/taps_cpp.git
        GIT_TAG main
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(taps_cpp)
    # taps_cpp's own CMakeLists.txt defines TAPS::static (src/CMakeLists.txt).
endif()

find_package(Threads REQUIRED)

# TLS scenarios (tls / tls_framed) need OpenSSL and the shared config header.
# The TAPS arm reaches TLS through taps::SecurityParameters, not the raw OpenSSL
# API, but still includes tls/tls_common.hpp for the pinned constants and prints
# the same TLS_IDENTITY line for the cross-arm comparability check.
find_package(OpenSSL REQUIRED)
set(TLS_COMMON_DIR "${PROJECT_SOURCE_DIR}/../tls")

if(TAPS_ENABLE_BENCHMARKS)
    find_package(benchmark REQUIRED)
endif()

add_subdirectory(tcpserver)
add_subdirectory(tcpserver_tls)
add_subdirectory(tcpclient)
add_subdirectory(udpserver)

if(TAPS_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()