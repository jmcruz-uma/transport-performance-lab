#!/usr/bin/env bash
set -euo pipefail

CLANG_C_COMPILER="clang-20"
CLANG_CXX_COMPILER="clang++-20"

GCC_C_COMPILER="gcc-14"
GCC_CXX_COMPILER="g++-14"

need_cmd() {
    command -v "$1" >/dev/null 2>&1
}

build_one() {
    local compiler="$1"
    local build_dir=""
    local compiler_label=""
    local c_compiler=""
    local cxx_compiler=""
    local use_libcxx="OFF"

    case "$compiler" in
      gcc)
        build_dir="build-gcc"
        compiler_label="GCC 14"
        c_compiler="$GCC_C_COMPILER"
        cxx_compiler="$GCC_CXX_COMPILER"
        ;;

      clang)
        build_dir="build-clang"
        compiler_label="Clang 20 (libc++)"
        c_compiler="$CLANG_C_COMPILER"
        cxx_compiler="$CLANG_CXX_COMPILER"
        use_libcxx="ON"
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

    # Google Benchmark comes from taps-asio/CMakeLists.txt now (FetchContent
    # for Clang, system find_package(benchmark) for GCC) -- same convention
    # every other project in this repo already uses. This used to build+
    # install its own copy into a hardcoded ~/Escritorio/TFM/ path outside
    # the repo: not portable to a fresh machine's home directory layout, and
    # its "already installed" cache check keyed only on compiler NAME, not
    # version, so switching clang-18 to clang-20 silently kept reusing a
    # clang-18-linked static library until caught by hand (found 2026-09-15
    # auditing the build ahead of the real-machine deployment).
    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$c_compiler" \
        -DCMAKE_CXX_COMPILER="$cxx_compiler" \
        -DTAPS_TCP_USE_LIBCXX="$use_libcxx" \
        -DTAPS_ENABLE_BENCHMARKS=ON \
        -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON \
        -DCMAKE_CXX_EXTENSIONS=OFF

    cmake --build "$build_dir" --config Release -j"$(nproc)" \
        --target tcpserver tcpserver_tls tcpserver_tls_framed udpserver tcpclient \
                  bench_tcp bench_tcp_whole bench_tcp_blocks bench_udp \
                  bench_tcp_tls bench_tcp_tls_framed

    echo ""
    echo "TAPS TCP Release build completed with $compiler_label."
    echo "TAPS fetched via FetchContent (see taps-asio/CMakeLists.txt)."
    echo "Google Benchmark fetched via FetchContent (Clang) or system find_package (GCC) -- see taps-asio/CMakeLists.txt."
    echo "Executables:"
    echo "  $build_dir/tcpserver/tcpserver"
    echo "  $build_dir/tcpclient/tcpclient"
    echo "  $build_dir/benchmarks/bench_tcp"
    echo ""
}

main() {
    if ! need_cmd cmake; then
        echo "Error: cmake is not installed"
        exit 1
    fi

    if ! need_cmd git; then
        echo "Error: git is not installed"
        exit 1
    fi

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
}

main "$@"
