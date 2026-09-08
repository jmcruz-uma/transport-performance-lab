#!/usr/bin/env bash
set -euo pipefail

BENCHMARK_REPO_DIR="${HOME}/Escritorio/TFM/google-benchmark-src"
BENCHMARK_INSTALL_GCC="${HOME}/Escritorio/TFM/google-benchmark-install-gcc"
BENCHMARK_INSTALL_CLANG="${HOME}/Escritorio/TFM/google-benchmark-install-clang"

CLANG_C_COMPILER="clang"
CLANG_CXX_COMPILER="clang++"

GCC_C_COMPILER="gcc-14"
GCC_CXX_COMPILER="g++-14"

need_cmd() {
    command -v "$1" >/dev/null 2>&1
}

ensure_google_benchmark_repo() {
    if [ -d "$BENCHMARK_REPO_DIR/.git" ]; then
        return
    fi

    rm -rf "$BENCHMARK_REPO_DIR"
    git clone https://github.com/google/benchmark.git "$BENCHMARK_REPO_DIR"

    if [ ! -d "$BENCHMARK_REPO_DIR/googletest/.git" ]; then
        git -C "$BENCHMARK_REPO_DIR" clone https://github.com/google/googletest.git googletest
    fi
}

ensure_benchmark_install() {
    local compiler="$1"
    local install_dir=""
    local build_dir=""
    local c_compiler=""
    local cxx_compiler=""
    local cxx_flags=""

    case "$compiler" in
      gcc)
        install_dir="$BENCHMARK_INSTALL_GCC"
        build_dir="$BENCHMARK_REPO_DIR/build-gcc"
        c_compiler="$GCC_C_COMPILER"
        cxx_compiler="$GCC_CXX_COMPILER"
        ;;
      clang)
        install_dir="$BENCHMARK_INSTALL_CLANG"
        build_dir="$BENCHMARK_REPO_DIR/build-clang"
        c_compiler="$CLANG_C_COMPILER"
        cxx_compiler="$CLANG_CXX_COMPILER"
        # Must match the harness's stdlib choice below, or linking the
        # static library into a libc++ binary breaks on std::string/vector ABI.
        cxx_flags="-stdlib=libc++"
        ;;
      *)
        echo "Unsupported compiler for benchmark: $compiler"
        exit 1
        ;;
    esac

    local config_file="$install_dir/lib/cmake/benchmark/benchmarkConfig.cmake"

    if [ -f "$config_file" ]; then
        echo "Using existing Google Benchmark install:"
        echo "  $install_dir"
        return
    fi

    echo "Google Benchmark not found for $compiler. Building it automatically..."

    ensure_google_benchmark_repo

    rm -rf "$build_dir"

    cmake -S "$BENCHMARK_REPO_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$c_compiler" \
        -DCMAKE_CXX_COMPILER="$cxx_compiler" \
        -DCMAKE_CXX_FLAGS="$cxx_flags" \
        -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_CXX_STANDARD_REQUIRED=ON \
        -DCMAKE_CXX_EXTENSIONS=OFF \
        -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
        -DBENCHMARK_DOWNLOAD_DEPENDENCIES=OFF \
        -DCMAKE_INSTALL_PREFIX="$install_dir"

    cmake --build "$build_dir" -j"$(nproc)"
    cmake --install "$build_dir"
}

build_one() {
    local compiler="$1"
    local build_dir=""
    local compiler_label=""
    local c_compiler=""
    local cxx_compiler=""
    local benchmark_prefix=""
    local benchmark_dir=""
    local use_libcxx="OFF"
    local extra_cmake_args=()

    case "$compiler" in
      gcc)
        build_dir="build-gcc"
        compiler_label="GCC 14"
        c_compiler="$GCC_C_COMPILER"
        cxx_compiler="$GCC_CXX_COMPILER"
        benchmark_prefix="$BENCHMARK_INSTALL_GCC"

        ensure_benchmark_install gcc
        ;;

      clang)
        build_dir="build-clang"
        compiler_label="Clang (libc++)"
        c_compiler="$CLANG_C_COMPILER"
        cxx_compiler="$CLANG_CXX_COMPILER"
        benchmark_prefix="$BENCHMARK_INSTALL_CLANG"
        use_libcxx="ON"

        ensure_benchmark_install clang
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

    benchmark_dir="$benchmark_prefix/lib/cmake/benchmark"

    extra_cmake_args=(
      -Dbenchmark_DIR="$benchmark_dir"
      -DTAPS_TCP_USE_LIBCXX="$use_libcxx"
      -DTAPS_ENABLE_BENCHMARKS=ON
      -DCMAKE_CXX_STANDARD=23
      -DCMAKE_CXX_STANDARD_REQUIRED=ON
      -DCMAKE_CXX_EXTENSIONS=OFF
    )

    rm -rf "$build_dir"

    cmake -S . -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$benchmark_prefix" \
        -DCMAKE_C_COMPILER="$c_compiler" \
        -DCMAKE_CXX_COMPILER="$cxx_compiler" \
        "${extra_cmake_args[@]}"

    cmake --build "$build_dir" --config Release -j"$(nproc)" \
        --target tcpserver tcpserver_tls tcpclient bench_tcp bench_tcp_tls

    echo ""
    echo "TAPS TCP Release build completed with $compiler_label."
    echo "TAPS fetched via FetchContent (see taps-asio/CMakeLists.txt)."
    echo "Using Google Benchmark from:"
    echo "  $benchmark_prefix"
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