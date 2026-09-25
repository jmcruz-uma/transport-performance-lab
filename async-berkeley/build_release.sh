#!/usr/bin/env bash
set -euo pipefail

build_one() {
    local compiler="$1"
    local build_dir=""
    local compiler_label=""
    local c_compiler=""
    local cxx_compiler=""
    local cxx_flags=""

    case "$compiler" in
      gcc)
        build_dir="build-gcc"
        compiler_label="GCC 14"
        c_compiler="gcc-14"
        cxx_compiler="g++-14"
        ;;
      clang)
        build_dir="build-clang"
        compiler_label="Clang 20 (libc++)"
        c_compiler="clang-20"
        cxx_compiler="clang++-20"
        # libc++ instead of the system's libstdc++: keeps energy/perf numbers
        # representative of clang's own standard library rather than GCC's,
        # and sidesteps clang picking up headers from whichever GCC version
        # happens to be newest on the machine (it broke against GCC 16 here).
        cxx_flags="-stdlib=libc++"
        ;;
      *)
        echo "Unsupported compiler: $compiler"
        exit 1
        ;;
    esac

    if ! command -v "$c_compiler" >/dev/null 2>&1; then
        echo "Error: $c_compiler is not installed or not available in PATH"
        exit 1
    fi

    if ! command -v "$cxx_compiler" >/dev/null 2>&1; then
        echo "Error: $cxx_compiler is not installed or not available in PATH"
        exit 1
    fi

    rm -rf "$build_dir"

    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$c_compiler" \
        -DCMAKE_CXX_COMPILER="$cxx_compiler" \
        -DCMAKE_CXX_FLAGS="$cxx_flags"

    cmake --build "$build_dir" --config Release -j"$(nproc)" \
        --target tcpserver tcpserver_framed tcpclient udpserver bench_tcp bench_tcp_whole bench_tcp_framed bench_udp

    echo ""
    echo "ASYNC-BERKELEY build completed in Release mode with $compiler_label."
    echo "Executables:"
    echo "  $build_dir/tcpserver/tcpserver"
    echo "  $build_dir/tcpclient/tcpclient"
    echo "  $build_dir/benchmarks/bench_tcp"
    echo ""
}

if [ $# -eq 0 ]; then
    build_one gcc
    build_one clang
elif [ $# -eq 1 ]; then
    case "$1" in
      gcc|clang)
        build_one "$1"
        ;;
      *)
        echo "Usage: $0 [gcc|clang]"
        exit 1
        ;;
    esac
else
    echo "Usage: $0 [gcc|clang]"
    exit 1
fi