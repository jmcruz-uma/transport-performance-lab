#!/usr/bin/env bash
# The one command for the measurement machine: preflight check -> build ->
# smoke test -> baseline campaign (loopback, every scenario) -> D7 netem
# RTT x loss sweep (netns+veth, every scenario) -> collect everything into
# global_results/. Meant to be started and left running unattended for a day
# or more -- every stage is logged to a timestamped file under
# campaign_logs/, and campaign_status.txt at the repo root always holds a
# one-line answer to "is it still going, and if not, why did it stop"
# without having to read the full log.
#
# Usage: sudo ./run_everything.sh
#
# Each stage aborts the whole run on failure (no point starting a multi-day
# sweep on top of a broken build) EXCEPT the smoke test, which is a warning,
# not a hard stop -- see STAGE 3 below for why.
#
# Why stage 6 (collection) is a separate script/stage, run again at the very
# end rather than relying on run.sh's own internal collection: run.sh (stage
# 4) only collects/builds master tables for whatever labels exist when IT
# finishes -- the loopback baseline. Stage 5 (the netem sweep) creates
# dozens/hundreds more labels afterwards; without a second collection pass
# after it, the entire D7 sweep -- the main reason this repo grew this
# tooling -- would sit correctly in each project's results/ but never reach
# global_results/ or get a master comparison table. Found 2026-09-15,
# auditing the deployment plan for gaps ahead of the real run.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAMP="$(date '+%Y%m%d_%H%M%S')"
LOG_DIR="$ROOT_DIR/campaign_logs/$STAMP"
LOG_FILE="$LOG_DIR/campaign.log"
STATUS_FILE="$ROOT_DIR/campaign_status.txt"

mkdir -p "$LOG_DIR"
# Mirror everything to the log file AND the terminal (useful if attached;
# irrelevant if not -- the log file is what matters for "I'll check tomorrow").
exec > >(tee -a "$LOG_FILE") 2>&1

CURRENT_STAGE="startup"

status() {
    printf '%s  [%s]  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" "$2" > "$STATUS_FILE"
}

log() { printf '\n[%s] ===== %s =====\n' "$(date '+%H:%M:%S')" "$*"; }

# ntfy.sh notification so the campaign can be watched without a terminal
# attached. Best-effort only -- `|| true` so a network hiccup here never
# masks the real stage failure or trips the ERR trap again.
NTFY_TOPIC="jmcruz-taps"
notify() {
    curl -s -m 10 -d "$1" "https://ntfy.sh/$NTFY_TOPIC" >/dev/null 2>&1 || true
}

on_error() {
    status "FAILED" "stage '$CURRENT_STAGE' failed -- see $LOG_FILE"
    log "ABORTED at stage: $CURRENT_STAGE"
    notify "❌ transport-performance-lab: FALLO en stage '$CURRENT_STAGE' -- ver $LOG_FILE"
    echo "See the tail of the log above for the actual error."
    echo "Nothing after this stage ran. Fix the problem, then rerun"
    echo "./run_everything.sh -- every stage (build, run.sh, the netem sweep,"
    echo "collection) is safe to rerun: builds are incremental, run.sh/netem resume by"
    echo "scenario via their own checkpoint files, and pre-existing results"
    echo "are never overwritten (moved aside with a timestamp instead)."
    exit 1
}
trap on_error ERR

need_root() {
    if [ "${EUID:-$(id -u)}" -ne 0 ]; then
        echo "Error: run as root (sudo ./run_everything.sh) -- RAPL, netns and"
        echo "cache-drop all need it, same as build.sh/run.sh already do."
        exit 1
    fi
}

# =========================
# STAGE 3 helper: smoke test every project actually runs, not just compiles.
# Uses the streaming scenario (present in all 5 projects, no TLS certs
# needed) over plain loopback -- this is a fast sanity check, not a
# measurement, so it deliberately does not use the netns topology (that
# path was already validated by hand across all 5 projects on 2026-09-14;
# see design/tls_experiment_notes.md D7). A failure here is a WARNING, not
# an abort: it's meant to catch an obviously broken build early and loud,
# but a flaky smoke-test iteration should not throw away a build that
# succeeded, so it does not stop run_everything.sh -- the failure is
# recorded prominently in the log either way.
# =========================
smoke_test_project() {
    local project="$1" compiler="$2"
    local dir="$ROOT_DIR/$project"
    local build_dir="$dir/build-$compiler"
    local server_bin="$build_dir/tcpserver/tcpserver"
    local bench_bin="$build_dir/benchmarks/bench_tcp"
    local port=19500

    if [ ! -x "$server_bin" ] || [ ! -x "$bench_bin" ]; then
        echo "  [$project/$compiler] SKIP -- binaries not found (build may only cover one compiler)"
        return 1
    fi

    local server_log
    server_log="$(mktemp)"
    (cd "$dir" && timeout 20 "./build-$compiler/tcpserver/tcpserver" "../files/100MB.bin" "$port" 1 > "$server_log" 2>&1) &
    local server_pid=$!
    sleep 1

    local out
    out="$(cd "$dir" && "./build-$compiler/benchmarks/bench_tcp" --server_port="$port" --benchmark_format=csv 2>/dev/null | tail -1)"

    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    rm -f "$server_log"

    # downloaded_bytes (the last CSV column) is emitted in scientific
    # notation by Google Benchmark's CSV reporter for large counters (e.g.
    # "1.04858e+08", not "104857600") -- a literal string match for
    # "104857600" never matches, which silently turned every single smoke
    # test into a false FAIL regardless of whether the transfer actually
    # worked (found 2026-09-15: all 5 projects x 2 compilers reported FAIL
    # here despite every one of them correctly transferring the file).
    # Parse the field numerically instead, with a tolerance for the CSV's
    # ~6-significant-figure rounding.
    local downloaded
    downloaded="$(echo "$out" | awk -F',' '{print $NF}' | tr -d '"')"
    if awk -v v="$downloaded" 'BEGIN { exit !(v >= 104857600 * 0.99) }' 2>/dev/null; then
        echo "  [$project/$compiler] OK -- 100 MiB transferred correctly ($downloaded bytes)"
        return 0
    else
        echo "  [$project/$compiler] FAIL -- unexpected output: $out"
        return 1
    fi
}

stage_smoke_test() {
    CURRENT_STAGE="smoke test"
    log "STAGE 3/6: Smoke test (streaming scenario, loopback, one quick real transfer per project)"
    local failures=0
    for project in asio taps-asio async-berkeley bsd-sockets capy-corosio; do
        for compiler in gcc clang; do
            if [ -d "$ROOT_DIR/$project/build-$compiler" ]; then
                smoke_test_project "$project" "$compiler" || failures=$((failures + 1))
            fi
        done
    done
    if [ "$failures" -gt 0 ]; then
        echo
        echo "WARNING: $failures smoke-test check(s) failed. The build succeeded"
        echo "(stage 2 would have aborted otherwise), but at least one binary did"
        echo "not produce a correct transfer at runtime. Review the log above."
        echo "Continuing anyway -- rerun with the log open if you want to stop"
        echo "and investigate instead of letting the full campaign start."
        sleep 10
    else
        log "Smoke test: all binaries transfer correctly."
    fi
}

main() {
    need_root
    log "Repository root: $ROOT_DIR"
    log "Log file: $LOG_FILE"
    status "RUNNING" "starting"

    CURRENT_STAGE="preflight"
    log "STAGE 1/6: Preflight (install + verify every prerequisite)"
    status "RUNNING" "stage 1/6: preflight"
    "$ROOT_DIR/preflight.sh"

    CURRENT_STAGE="build"
    log "STAGE 2/6: Build (all 5 projects, both compilers)"
    status "RUNNING" "stage 2/6: build"
    "$ROOT_DIR/build.sh"

    status "RUNNING" "stage 3/6: smoke test"
    stage_smoke_test

    CURRENT_STAGE="baseline campaign (run.sh)"
    log "STAGE 4/6: Baseline campaign -- every scenario, loopback (RTT=0, no netem)"
    status "RUNNING" "stage 4/6: baseline campaign (run.sh) -- this is a long one, check campaign_logs/$STAMP/campaign.log for progress"
    "$ROOT_DIR/run.sh"

    CURRENT_STAGE="D7 netem sweep (run_rtt_sweep.sh)"
    log "STAGE 5/6: D7 network-realism sweep -- every scenario x RTT x loss grid (netns+veth)"
    status "RUNNING" "stage 5/6: D7 netem sweep -- this is the long one, check campaign_logs/$STAMP/campaign.log for progress"
    "$ROOT_DIR/netem/run_rtt_sweep.sh"

    CURRENT_STAGE="collect global results (collect_global_results.sh)"
    log "STAGE 6/6: Collecting every label (baseline + every D7 grid point) into global_results/"
    status "RUNNING" "stage 6/6: collecting global results and building master tables"
    "$ROOT_DIR/collect_global_results.sh"

    log "ALL STAGES COMPLETE"
    status "DONE" "finished at $(date '+%Y-%m-%d %H:%M:%S') -- log: $LOG_FILE"
    notify "✅ transport-performance-lab: campaña D7 terminada OK ($(date '+%Y-%m-%d %H:%M:%S'))"
}

main "$@"
