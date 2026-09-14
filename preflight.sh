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

    # universe must be enabled for g++-14/clang-18/libc++-18 on a minimal
    # (e.g. server ISO) Ubuntu 24.04 install -- desktop installs usually have
    # it already, but this is cheap and idempotent either way.
    if command -v add-apt-repository >/dev/null 2>&1; then
        add-apt-repository -y universe >/dev/null 2>&1 || true
        apt-get update -qq
    fi

    local packages=(
        build-essential git cmake make pkg-config
        gcc-14 g++-14
        clang-18 clang
        libc++-18-dev libc++abi-18-dev
        libssl-dev openssl
        iproute2 ethtool
        python3 python3-pip python3-venv python3-matplotlib
        linux-tools-common linux-tools-generic
    )
    apt-get install -y "${packages[@]}"

    # pypdf is not always packaged; build.sh already falls back to pip for it,
    # so this is best-effort only, not a hard requirement here.
    apt-get install -y python3-pypdf 2>/dev/null || true

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
    check_cmd clang-18
    check_cmd clang++-18 || true
    # "clang"/"clang++" must resolve to the -18 toolchain, since every
    # build_release.sh invokes them by the bare name.
    if command -v clang >/dev/null 2>&1; then
        local v
        v="$(clang --version | head -1)"
        if echo "$v" | grep -q "18\."; then
            ok "clang resolves to a clang-18 build ($v)"
        else
            fail "clang on PATH is not clang-18 ($v) -- every project's build_release.sh calls 'clang'/'clang++' by bare name"
        fi
    else
        fail "'clang' not found on PATH (need it as the bare command, not just clang-18)"
    fi
}

check_libcxx() {
    log "Checking libc++..."
    if dpkg -s libc++-18-dev >/dev/null 2>&1 && dpkg -s libc++abi-18-dev >/dev/null 2>&1; then
        ok "libc++-18-dev and libc++abi-18-dev installed"
    else
        fail "libc++-18-dev / libc++abi-18-dev missing (every clang build uses -stdlib=libc++)"
    fi
}

check_openssl() {
    log "Checking OpenSSL..."
    if dpkg -s libssl-dev >/dev/null 2>&1; then
        ok "libssl-dev installed ($(openssl version 2>/dev/null))"
    else
        fail "libssl-dev missing (needed by every tls/tls_framed scenario, all 4 arms that have TLS)"
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

    if ! ip link add __preflight_veth0 type veth peer name __preflight_veth1 2>/dev/null; then
        fail "cannot create a veth pair ('ip link add ... type veth' failed)"
    else
        ip link del __preflight_veth0 2>/dev/null
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
    check_cmake_version
    check_netem_netns
    check_rapl
    check_network
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
