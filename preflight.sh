#!/usr/bin/env bash
# One-shot readiness check for the measurement machine, meant to be run BEFORE
# a long unattended campaign (run_everything.sh) so a missing dependency is a
# 2-minute fix now, not a surprise discovered two days later. Idempotent:
# installs whatever is missing via apt, then verifies every capability the
# harness actually needs actually works (not just "package is installed" --
# e.g. RAPL must be READABLE, netns must actually be usable, GitHub must
# actually be reachable). Exits non-zero and prints a clear summary of every
# failed check if anything is still broken after the install pass.
#
# Usage: sudo ./preflight.sh
set -uo pipefail   # NOT -e: we want to run every check and report all
                    # failures at once, not stop at the first one.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAILURES=()
WARNINGS=()

log()  { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAILURES+=("$*"); }
warn() { printf '  \033[33mWARN\033[0m %s\n' "$*"; WARNINGS+=("$*"); }

need_root() {
    if [ "${EUID:-$(id -u)}" -ne 0 ]; then
        echo "Error: run this as root (sudo ./preflight.sh) -- the real campaign"
        echo "(build.sh / run.sh / netem sweep) is run as root too (RAPL, netns,"
        echo "cache-drop all need it), so preflight checks the same way."
        exit 1
    fi
}

# =========================
# STAGE 1: install every apt package the harness needs
# =========================
install_packages() {
    log "Installing/updating required apt packages (idempotent)..."
    apt-get update -qq

    # universe must be enabled for g++-14/clang-20/libc++-20 on a minimal
    # (e.g. server ISO) Ubuntu 24.04 install -- desktop installs usually have
    # it already, but this is cheap and idempotent either way.
    if command -v add-apt-repository >/dev/null 2>&1; then
        add-apt-repository -y universe >/dev/null 2>&1 || true
        apt-get update -qq
    fi

    local packages=(
        build-essential git cmake make pkg-config
        gcc-14 g++-14
        clang-20
        libc++-20-dev libc++abi-20-dev
        libssl-dev openssl
        libbenchmark-dev
        iproute2 ethtool curl
        python3 python3-pip python3-venv python3-matplotlib
        linux-tools-common linux-tools-generic
    )
    apt-get install -y "${packages[@]}"

    # pypdf is what merge.py (run.sh's last step, after a potentially
    # multi-day campaign) needs to merge PDF reports; run.sh has `set -e`, so
    # a missing pypdf there means an unhandled Python traceback aborting the
    # run right at the finish line. Confirmed on Ubuntu 24.04 (noble):
    # python3-pypdf 4.0.2-1 is a real package in the standard archive, so
    # this should always succeed here -- but mirror build.sh's apt-then-pip
    # fallback anyway, and treat total failure as fatal, so this is caught
    # now rather than discovered as a crash two days into the campaign.
    if ! python3 -c 'import pypdf' >/dev/null 2>&1; then
        if ! apt-get install -y python3-pypdf 2>/dev/null; then
            # Ubuntu 24.04's system Python is "externally managed" (PEP 668);
            # a plain `pip install --user` refuses to run at all here, not
            # just warns -- confirmed hitting this for real on WSL2 while
            # testing. --break-system-packages is the documented override,
            # safe for a --user install into this one account, not the
            # system site-packages.
            python3 -m pip install --user pypdf ||
            python3 -m pip install --user --break-system-packages pypdf || {
                echo "Error: could not install the 'pypdf' Python module via apt or pip." >&2
                echo "merge.py (run.sh's final step) needs it; fix this before proceeding." >&2
                exit 1
            }
        fi
    fi

    modprobe sch_netem 2>/dev/null || true
}

# =========================
# STAGE 2: verify every capability actually works
# =========================
check_cmd() {
    local cmd="$1" label="${2:-$1}"
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "$label found ($(command -v "$cmd"))"
    else
        fail "$label not found on PATH"
    fi
}

check_compilers() {
    log "Checking compilers..."
    check_cmd gcc-14
    check_cmd g++-14
    # Every build_release.sh invokes clang by the exact versioned command
    # (clang-20/clang++-20), never the bare 'clang'/'clang++' name -- found
    # 2026-09-15 that the bare name is NOT a reliable pin: '-stdlib=libc++'
    # resolves headers via the version-agnostic /usr/include/c++/v1 symlink,
    # which apt repoints to whichever libc++-N-dev was installed/upgraded
    # most recently, regardless of which clang binary you actually invoke.
    # With only libc++-18-dev installed that accidentally happened to match
    # clang-18; once libc++-20-dev is also installed (needed for
    # capy-corosio/async-berkeley -- see below), a bare 'clang' invocation
    # would silently compile with a DIFFERENT libc++ than its own bundled
    # one. Pinning both the compiler and the -dev package to the same
    # version (20) sidesteps the ambiguity entirely instead of chasing it.
    check_cmd clang-20
    check_cmd clang++-20
}

check_libcxx() {
    log "Checking libc++..."
    if dpkg -s libc++-20-dev >/dev/null 2>&1 && dpkg -s libc++abi-20-dev >/dev/null 2>&1; then
        ok "libc++-20-dev and libc++abi-20-dev installed"
    else
        fail "libc++-20-dev / libc++abi-20-dev missing (every clang build uses -stdlib=libc++)"
        return
    fi

    # Not just "is the package installed" -- actually compile the two C++20
    # library features capy-corosio and async-berkeley need (std::stop_token,
    # and operator<=> on a std::vector iterator) through the exact
    # clang-20 + -stdlib=libc++ invocation build_release.sh uses. clang-18's
    # libc++ genuinely lacks both (confirmed 2026-09-15, not fixable by any
    # flag); this catches that class of gap directly instead of trusting
    # version numbers to imply capability.
    local tmp_src tmp_obj
    tmp_src="$(mktemp --suffix=.cpp)"
    tmp_obj="$(mktemp)"
    cat > "$tmp_src" <<'EOF'
#include <vector>
#include <stop_token>
int main() {
    std::vector<int> v{1, 2, 3};
    auto cmp = v.begin() <=> v.end();
    std::stop_token st;
    (void)cmp; (void)st;
}
EOF
    if clang++-20 -std=c++23 -stdlib=libc++ -c "$tmp_src" -o "$tmp_obj" 2>/dev/null; then
        ok "clang-20 -stdlib=libc++ actually compiles std::stop_token and vector-iterator operator<=>"
    else
        fail "clang-20 -stdlib=libc++ cannot compile std::stop_token / vector-iterator operator<=>" \
             "-- capy-corosio and async-berkeley's clang builds need both. Re-run" \
             "'clang++-20 -std=c++23 -stdlib=libc++ -c $tmp_src' by hand to see the actual error."
    fi
    rm -f "$tmp_src" "$tmp_obj"
}

check_openssl() {
    log "Checking OpenSSL..."
    if dpkg -s libssl-dev >/dev/null 2>&1; then
        ok "libssl-dev installed ($(openssl version 2>/dev/null))"
    else
        fail "libssl-dev missing (needed by every tls/tls_framed scenario, all 4 arms that have TLS)"
    fi
}

check_google_benchmark() {
    log "Checking Google Benchmark (system package, used by every project's GCC build)..."
    # Every project's GCC build resolves Google Benchmark via
    # find_package(benchmark REQUIRED) against the system package (the Clang
    # build FetchContents its own copy instead -- see the comment in any
    # benchmarks/CMakeLists.txt for why). Nothing in build.sh/preflight.sh
    # used to install this: it only ever worked because libbenchmark-dev
    # happened to already be present on this development machine from
    # unrelated earlier work, which masked a real gap here (found 2026-09-15
    # auditing the build ahead of the real-machine deployment -- a genuinely
    # fresh Ubuntu 24.04 install would have failed to configure asio,
    # async-berkeley, bsd-sockets, capy-corosio and taps-asio's GCC builds
    # with "Could not find benchmark").
    if dpkg -s libbenchmark-dev >/dev/null 2>&1 && \
       dpkg -L libbenchmark-dev 2>/dev/null | grep -q 'cmake/benchmark/benchmarkConfig\.cmake$'; then
        ok "libbenchmark-dev installed and find_package(benchmark) resolvable"
    else
        fail "libbenchmark-dev missing or its CMake config not found -- every project's GCC build needs this (find_package(benchmark REQUIRED))"
    fi
}

check_cmake_version() {
    log "Checking cmake version..."
    if command -v cmake >/dev/null 2>&1; then
        local v maj min
        v="$(cmake --version | head -1 | awk '{print $3}')"
        maj="$(echo "$v" | cut -d. -f1)"
        min="$(echo "$v" | cut -d. -f2)"
        if [ "$maj" -gt 3 ] || { [ "$maj" -eq 3 ] && [ "$min" -ge 20 ]; }; then
            ok "cmake $v (>= 3.20 required by taps-asio/CMakeLists.txt)"
        else
            fail "cmake $v is older than the 3.20 taps-asio requires"
        fi
    else
        fail "cmake not found"
    fi
}

check_netem_netns() {
    log "Checking network namespace + netem support (creates and destroys a throwaway test namespace)..."
    if ! ip netns add __preflight_test 2>/dev/null; then
        fail "cannot create a network namespace ('ip netns add' failed) -- is this a container without CAP_NET_ADMIN / CAP_SYS_ADMIN?"
        return
    fi
    ip netns del __preflight_test 2>/dev/null

    # Interface names are capped at IFNAMSIZ (15 chars + NUL) by the kernel;
    # __preflight_veth0/1 (17 chars) silently fails 'ip link add' with that
    # limit exceeded -- found for real 2026-09-15 on the measurement machine.
    # veth-host/veth-peer, what the actual netem topology uses (see
    # netem/netem_common.sh), are 9 chars and were never affected.
    if ! ip link add __pf_veth0 type veth peer name __pf_veth1 2>/dev/null; then
        fail "cannot create a veth pair ('ip link add ... type veth' failed)"
    else
        ip link del __pf_veth0 2>/dev/null
        ok "network namespaces + veth pairs work"
    fi

    if lsmod | grep -q '^sch_netem' || modinfo sch_netem >/dev/null 2>&1; then
        ok "sch_netem (netem qdisc) available"
    else
        fail "sch_netem module not available -- 'modprobe sch_netem' failed and it's not built in"
    fi

    if tc qdisc add dev lo root netem delay 1ms >/dev/null 2>&1; then
        ok "netem qdisc can actually be applied"
        tc qdisc del dev lo root >/dev/null 2>&1
    else
        fail "netem qdisc could not be applied even though the module is present"
    fi
}

check_rapl() {
    log "Checking RAPL energy readout (the whole reason this runs on real hardware)..."
    local rapl_path="/sys/class/powercap/intel-rapl:0/energy_uj"
    if [ ! -e "$rapl_path" ]; then
        fail "$rapl_path does not exist. This machine's CPU/kernel doesn't expose RAPL," \
             "or the intel_rapl_msr/intel_rapl_common modules aren't loaded (try: modprobe intel_rapl_msr)." \
             "Every campaign's energy numbers depend on this -- do not proceed without it."
        return
    fi
    if [ -r "$rapl_path" ]; then
        local val
        val="$(cat "$rapl_path" 2>/dev/null)"
        ok "RAPL readable: $rapl_path = ${val} uJ"
    else
        fail "$rapl_path exists but is not readable even as root (unexpected -- check kernel lockdown/MAC policy)"
    fi
}

check_network() {
    log "Checking network access to GitHub (every project FetchContents its dependencies from there)..."
    if command -v curl >/dev/null 2>&1; then
        if curl -fsS --max-time 10 -o /dev/null https://github.com; then
            ok "github.com reachable"
        else
            fail "cannot reach https://github.com -- FetchContent (asio, Google Benchmark, corosio, capy, taps_cpp) needs this"
        fi
    else
        if command -v git >/dev/null 2>&1 && git ls-remote https://github.com/git/git >/dev/null 2>&1; then
            ok "github.com reachable (via git ls-remote)"
        else
            warn "could not verify GitHub reachability (no curl, and git ls-remote failed or git missing) -- verify manually"
        fi
    fi
}

check_curl() {
    log "Checking curl (run_everything.sh notifies ntfy.sh with it when the campaign ends)..."
    check_cmd curl
}

check_disk_space() {
    log "Checking free disk space..."
    local avail_kb
    avail_kb="$(df --output=avail -k "$ROOT_DIR" | tail -1 | tr -d ' ')"
    local avail_gb=$((avail_kb / 1024 / 1024))
    if [ "$avail_gb" -ge 15 ]; then
        ok "${avail_gb} GiB free (>= 15 GiB recommended: 2 compilers x 5 projects x FetchContent sources, plus days of results/logs/plots)"
    else
        warn "${avail_gb} GiB free -- may not be enough for a multi-day campaign's accumulated results/plots/PDFs. 15+ GiB recommended."
    fi
}

check_python_modules() {
    log "Checking Python modules used by the reporting pipeline..."
    if python3 -c 'import matplotlib' 2>/dev/null; then
        ok "python3 matplotlib available"
    else
        fail "python3 matplotlib not importable (build.sh installs this too, but preflight checks it explicitly)"
    fi
    if python3 -c 'import pypdf' 2>/dev/null; then
        ok "python3 pypdf available"
    else
        warn "python3 pypdf not importable yet -- build.sh will try to install it (apt or pip fallback)"
    fi
}

check_build_scripts_present() {
    log "Checking the repo itself is the expected shape..."
    local missing=0
    for f in build.sh run.sh netem/netem_common.sh netem/run_rtt_sweep.sh; do
        if [ ! -f "$ROOT_DIR/$f" ]; then
            fail "$f not found at repo root ($ROOT_DIR) -- wrong directory, or repo not fully checked out?"
            missing=1
        fi
    done
    [ "$missing" -eq 0 ] && ok "build.sh / run.sh / netem/*.sh all present"
}

main() {
    need_root
    log "Repository root: $ROOT_DIR"
    log "===== STAGE 1: installing packages ====="
    install_packages

    log "===== STAGE 2: verifying every capability ====="
    check_build_scripts_present
    check_compilers
    check_libcxx
    check_openssl
    check_google_benchmark
    check_cmake_version
    check_netem_netns
    check_rapl
    check_network
    check_curl
    check_disk_space
    check_python_modules

    echo
    log "===== SUMMARY ====="
    if [ ${#WARNINGS[@]} -gt 0 ]; then
        echo "Warnings (non-fatal, but worth a look):"
        for w in "${WARNINGS[@]}"; do echo "  - $w"; done
    fi

    if [ ${#FAILURES[@]} -eq 0 ]; then
        log "All checks passed. Safe to run ./run_everything.sh."
        exit 0
    else
        echo
        echo "FAILED (${#FAILURES[@]}) -- fix these before starting an unattended campaign:"
        for f in "${FAILURES[@]}"; do echo "  - $f"; done
        exit 1
    fi
}

main "$@"
