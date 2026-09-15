#!/usr/bin/env bash
# D7 -- network realism: re-run every scenario across a matrix of simulated
# round-trip times AND packet-loss rates, instead of loopback-only (see
# design/receive_path_redesign.md D7 and design/tls_experiment_notes.md;
# motivated concretely by a 2026-09-14 debugging session that found a small
# TAPS-vs-asio TLS delta at RTT=0 that flips sign entirely once ~1.7ms of RTT
# is present -- a loopback-only campaign would have reported an artifact as
# if it were the real result). Traffic is carried over a Linux network
# namespace + veth pair (see netem_common.sh), not shaped directly on `lo`,
# so every scenario -- not just the TLS ones -- gets a genuinely separate
# client/server network stack and can be shaped per direction.
#
# What it does, once at the start:
#   1. creates the netns+veth topology (netns_setup, idempotent)
# ...then per (RTT, loss) point in the NETEM_RTTS_MS x NETEM_LOSS_PCT grid:
#   2. shapes both veth endpoints with netem (delay + bandwidth cap + Simple
#      Gilbert loss -- see netem_common.sh for why the rate cap is mandatory
#      and how the loss parameters are derived)
#   3. verifies the RTT shaping actually took effect (ping across the veth
#      link), refusing to silently record a mislabelled result if it didn't
#   4. runs NETEM_SCENARIOS for each project in NETEM_PROJECTS (same
#      per-project scripts/run_bench.py every other campaign uses, now
#      reading NETEM_SERVER_HOST/NETEM_SERVER_NETNS -- exported by
#      netem_common.sh -- to launch the server inside the namespace and
#      point the client at its veth IP; nothing project-specific here)
#   5. moves each project's results/<scenario>/ into
#      results/<scenario>__netem_rtt_<R>ms_loss_<L>pct/ so repeated points
#      never collide and a plain (non-netem) campaign's results are never
#      touched
# ...and once at the end (also on any error/interrupt, via trap):
#   6. tears down the netns+veth topology
#
# Env config (all optional):
#   NETEM_RTTS_MS        target RTTs in ms, space-separated (default: "0 1 5 10 20 50")
#   NETEM_LOSS_PCT        target average loss rates in %, space-separated
#                          (default: "0 0.1 1 5" -- see netem_common.sh for the
#                          Simple Gilbert derivation; combined with NETEM_RTTS_MS
#                          as a full factorial grid)
#   NETEM_MEAN_BURST_PKTS  mean consecutive packets per loss event (default: 3,
#                           see netem_common.sh)
#   NETEM_SCENARIOS       RUN_SCENARIOS value passed to each run_bench.py
#                          (default: every scenario in bench_scenarios.py --
#                          streaming, whole_object, blocks, tls, tls_framed,
#                          udp_k64, udp_k1400; narrow this to iterate faster)
#   NETEM_PROJECTS        which subprojects to sweep (default: same 5 as run.sh)
#   NETEM_RATE_MBIT        bandwidth cap paired with the delay (default: 1000, see netem_common.sh)
#   NETEM_LIMIT_PKTS       netem queue depth (default: 50000, see netem_common.sh)
#   NETEM_RTT_TOLERANCE_MS how far the measured RTT may drift from the target
#                          before a point is aborted (default: 2)
#   DRY_RUN=1              plumbing-only: print the plan, touch no qdisc/netns,
#                          run nothing
#
# Usage:
#   sudo ./netem/run_rtt_sweep.sh
#   NETEM_RTTS_MS="0 1 10" NETEM_LOSS_PCT="0 1" NETEM_SCENARIOS="tls" ./netem/run_rtt_sweep.sh
#
# Running as root is not required if build.sh has been run at least once (it
# installs a NOPASSWD sudo rule scoped to the tc and ip binaries specifically).
set -eu

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$HERE")"
# shellcheck source=./netem_common.sh
source "$HERE/netem_common.sh"

NETEM_RTTS_MS="${NETEM_RTTS_MS:-0 1 5 10 20 50}"
NETEM_LOSS_PCT="${NETEM_LOSS_PCT:-0 0.1 1 5}"
# Every scenario bench_scenarios.py defines. Keep this list in sync with that
# file's SCENARIOS dict if a scenario is ever added/renamed there.
NETEM_SCENARIOS="${NETEM_SCENARIOS:-streaming whole_object blocks tls tls_framed udp_k64 udp_k1400}"
NETEM_PROJECTS="${NETEM_PROJECTS:-asio taps-asio async-berkeley bsd-sockets capy-corosio}"
NETEM_RTT_TOLERANCE_MS="${NETEM_RTT_TOLERANCE_MS:-2}"
DRY_RUN="${DRY_RUN:-}"

MANIFEST_DIR="$ROOT_DIR/results_netem"
mkdir -p "$MANIFEST_DIR"

# Points/projects where run_bench.py failed -- tracked instead of aborting
# immediately (see run_point() below) so one bad grid point out of 24 doesn't
# cost the rest of a multi-day sweep.
SWEEP_FAILURES=()

log() {
    printf '\n[%s] [netem-sweep] %s\n' "$(date '+%H:%M:%S')" "$*"
}

cleanup() {
    log "Cleaning up: tearing down the netns+veth topology"
    netem_clear_link || true
    netns_teardown || true
}
trap cleanup EXIT INT TERM

# Never silently overwrite a pre-existing (non-netem) results/<scenario>
# directory -- move it aside once, loudly, the first time this sweep touches it.
stash_preexisting_results() {
    local project_dir="$1" scenario="$2"
    local results_path="$ROOT_DIR/$project_dir/results/$scenario"
    if [ -e "$results_path" ]; then
        local backup="${results_path}__preexisting_backup_$(date '+%Y%m%d_%H%M%S')"
        log "WARNING: $results_path already exists (from a prior non-netem run)."
        log "  Moving it aside to: $backup"
        log "  (never deleted -- recover it manually if you need it)"
        mv "$results_path" "$backup"
    fi
}

relocate_results() {
    local project_dir="$1" scenario="$2" rtt_ms="$3" loss_pct="$4"
    local results_path="$ROOT_DIR/$project_dir/results/$scenario"
    local suffix="__netem_rtt_${rtt_ms}ms_loss_${loss_pct}pct"
    local dest="$ROOT_DIR/$project_dir/results/${scenario}${suffix}"

    if [ ! -d "$results_path" ]; then
        log "No results/$scenario produced for $project_dir (scenario not built, or run failed) -- nothing to relocate"
        return
    fi

    if [ -e "$dest" ]; then
        local backup="${dest}__superseded_$(date '+%Y%m%d_%H%M%S')"
        log "WARNING: $dest already exists (from a previous sweep run at this same grid point)."
        log "  Moving it aside to: $backup (never deleted -- recover it manually if you need it)"
        mv "$dest" "$backup"
    fi
    mv "$results_path" "$dest"
    log "Relocated $project_dir/results/$scenario -> $project_dir/results/${scenario}${suffix}"
}

# Cross-cutting realism knobs D7 also asks to document (RTT/loss are the
# variables under test here; these are recorded, not changed, so a reader can
# rule out "the delta is actually an untuned kernel setting").
capture_tcp_environment() {
    local rtt_ms="$1" loss_pct="$2"
    local out="$MANIFEST_DIR/tcp_env_rtt_${rtt_ms}ms_loss_${loss_pct}pct.txt"
    {
        echo "==== netem grid point: RTT=${rtt_ms}ms loss=${loss_pct}% ===="
        date '+%Y-%m-%d %H:%M:%S'
        echo
        echo "-- tc qdisc on $VETH_HOST (root ns) --"
        sudo tc qdisc show dev "$VETH_HOST" 2>&1 || true
        echo
        echo "-- tc qdisc on $VETH_PEER ($NETNS_NAME) --"
        sudo ip netns exec "$NETNS_NAME" tc qdisc show dev "$VETH_PEER" 2>&1 || true
        echo
        echo "-- tcp_congestion_control --"
        sysctl net.ipv4.tcp_congestion_control 2>&1 || true
        echo
        echo "-- tcp_rmem / tcp_wmem --"
        sysctl net.ipv4.tcp_rmem net.ipv4.tcp_wmem 2>&1 || true
        echo
        echo "-- TCP_NODELAY usage in this codebase (none found = Nagle is on everywhere, symmetric) --"
        grep -rln "NODELAY\|no_delay" "$ROOT_DIR"/*/tcpserver* "$ROOT_DIR"/*/benchmarks 2>/dev/null || echo "(none found)"
        echo
        echo "-- offload flags on $VETH_HOST / $VETH_PEER (best-effort) --"
        ethtool -k "$VETH_HOST" 2>&1 || echo "(ethtool not available or not applicable)"
    } > "$out"
    log "TCP environment snapshot: $out"
}

run_point() {
    local rtt_ms="$1" loss_pct="$2"

    log "===== Grid point: RTT=${rtt_ms}ms loss=${loss_pct}% ====="

    if [ -n "$DRY_RUN" ]; then
        log "[DRY_RUN] would set RTT=${rtt_ms}ms loss=${loss_pct}%, run scenarios [$NETEM_SCENARIOS]" \
            "for projects [$NETEM_PROJECTS], then clear the shaping"
        return
    fi

    netem_set_link "$rtt_ms" "$loss_pct" "$NETEM_MEAN_BURST_PKTS"
    if [ "$rtt_ms" != "0" ]; then
        netem_verify_rtt "$rtt_ms" "$NETEM_RTT_TOLERANCE_MS"
    fi
    capture_tcp_environment "$rtt_ms" "$loss_pct"

    for project_dir in $NETEM_PROJECTS; do
        local full_dir="$ROOT_DIR/$project_dir"
        local run_script="$full_dir/scripts/run_bench.py"

        if [ ! -f "$run_script" ]; then
            log "Skipping $project_dir: scripts/run_bench.py not found"
            continue
        fi

        for scenario in $NETEM_SCENARIOS; do
            stash_preexisting_results "$project_dir" "$scenario"
        done

        log "Running [$NETEM_SCENARIOS] for $project_dir at RTT=${rtt_ms}ms loss=${loss_pct}%"
        # A crash here (e.g. a project missing binaries for one of the
        # requested scenarios) must not abort the whole 24-point sweep via
        # `set -e` -- that would silently lose every remaining grid point,
        # possibly days of measurement, over one bad combination. Recorded
        # and reported at the end instead; this point/project's own results
        # (if partial) are still collected by relocate_results below.
        if ! ( cd "$full_dir" && RUN_SCENARIOS="$NETEM_SCENARIOS" python3 scripts/run_bench.py ); then
            log "ERROR: run_bench.py failed for $project_dir at RTT=${rtt_ms}ms loss=${loss_pct}% -- see the traceback above."
            log "Continuing with the remaining projects/grid points instead of losing the rest of the sweep."
            SWEEP_FAILURES+=("$project_dir @ RTT=${rtt_ms}ms loss=${loss_pct}%")
        fi

        for scenario in $NETEM_SCENARIOS; do
            relocate_results "$project_dir" "$scenario" "$rtt_ms" "$loss_pct"
        done
    done

    netem_clear_link
}

main() {
    log "Repository root: $ROOT_DIR"
    log "RTT points: $NETEM_RTTS_MS"
    log "Loss points: $NETEM_LOSS_PCT"
    log "Grid size: $(echo $NETEM_RTTS_MS | wc -w) x $(echo $NETEM_LOSS_PCT | wc -w) = $(( $(echo $NETEM_RTTS_MS | wc -w) * $(echo $NETEM_LOSS_PCT | wc -w) )) points"
    log "Scenarios: $NETEM_SCENARIOS"
    log "Projects: $NETEM_PROJECTS"

    if [ -z "$DRY_RUN" ]; then
        netem_require
        netns_setup
    fi

    for rtt_ms in $NETEM_RTTS_MS; do
        for loss_pct in $NETEM_LOSS_PCT; do
            run_point "$rtt_ms" "$loss_pct"
        done
    done

    log "Sweep finished. Per-project results under <project>/results/<scenario>__netem_rtt_<R>ms_loss_<L>pct/"
    log "TCP environment snapshots under $MANIFEST_DIR/"

    if [ "${#SWEEP_FAILURES[@]}" -gt 0 ]; then
        log "WARNING: ${#SWEEP_FAILURES[@]} (project, grid point) combination(s) failed during the sweep:"
        local f
        for f in "${SWEEP_FAILURES[@]}"; do
            log "  - $f"
        done
        log "Every other combination was still attempted -- this failure is reported, not silent or total."
        exit 1
    fi
}

main "$@"
