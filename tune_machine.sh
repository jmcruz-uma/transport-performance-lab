#!/usr/bin/env bash
# System-level tuning for low-variance, RAPL-clean benchmarking (Tier 1 only --
# no reboot, no changes to run_bench.py or any experiment script; see
# design/tls_experiment_notes.md and the README for the Tier 2 discussion --
# CPU isolation + taskset pinning -- that was deliberately deferred).
#
# Standalone and separate from run_everything.sh on purpose: applying this
# is a real methodological choice (it trades "representative of a normally
# configured machine" for "low-variance, comparable across arms"), not
# something that should happen silently as a side effect of running the
# campaign. Run it once, by hand, before run_everything.sh.
#
# Rationale (see the README's "Machine tuning" section for the full
# discussion and citations -- Mytkowicz et al., ASPLOS 2009, on how
# innocuous-looking environment differences bias benchmark comparisons):
#   - CPU governor=performance, Turbo Boost off: the two biggest sources of
#     run-to-run POWER variance -- directly contaminates every RAPL energy
#     number this project's whole methodology depends on, not just timing.
#   - SMT off: removes sibling-thread cache/execution-unit contention as a
#     noise source.
#   - ASLR off, NMI watchdog off, THP off: standard, well-documented
#     variance reducers (Mytkowicz et al. is literally about ASLR-adjacent
#     effects like this).
#   - swap off: nothing should ever page during a run.
#   - TSC clocksource: highest-resolution, lowest-overhead timekeeping
#     available; avoids falling back to a slower/jitterier clocksource.
#
# Every change is undoable: this script snapshots the machine's ORIGINAL
# values before touching anything, so `./tune_machine.sh restore` puts
# everything back exactly as found -- not just "sane defaults".
#
# Usage:
#   sudo ./tune_machine.sh apply     # default if no argument given
#   sudo ./tune_machine.sh restore
#   sudo ./tune_machine.sh status    # show current values, change nothing
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_FILE="$ROOT_DIR/.tune_machine_state"
MODE="${1:-apply}"

log()  { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
ok()   { printf '  \033[32mOK\033[0m   %s\n' "$*"; }
skip() { printf '  \033[33mSKIP\033[0m %s\n' "$*"; }
note() { printf '  \033[36m--\033[0m   %s\n' "$*"; }
FAIL_COUNT=0
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

need_root() {
    if [ "${EUID:-$(id -u)}" -ne 0 ]; then
        echo "Error: run as root (sudo ./tune_machine.sh $MODE)"
        exit 1
    fi
}

# =========================
# State file: one KEY=value per tunable actually changed, so restore only
# ever touches what apply actually touched (never invents a "default").
# =========================
declare -A ORIG   # in-memory during apply, written out at the end
state_save() {
    : > "$STATE_FILE.tmp"
    for k in "${!ORIG[@]}"; do
        printf '%s=%s\n' "$k" "${ORIG[$k]}" >> "$STATE_FILE.tmp"
    done
    mv "$STATE_FILE.tmp" "$STATE_FILE"
    chmod 600 "$STATE_FILE"
}
state_load() {
    [ -f "$STATE_FILE" ] || { echo "No $STATE_FILE found -- nothing to restore (has apply ever run?)"; exit 1; }
    # shellcheck disable=SC1090
    source <(sed 's/^/ORIG_/' "$STATE_FILE")
}

# =========================
# Individual tunables. Each apply_* function records the ORIGINAL value into
# ORIG[...] before changing anything; each restore_* function reads it back
# from the sourced state file (ORIG_<key> after state_load's sed prefix).
# =========================

apply_governor() {
    local base=/sys/devices/system/cpu/cpu0/cpufreq
    if [ ! -d "$base" ]; then
        skip "CPU governor: no cpufreq sysfs (virtualized CPU? not exposed here) -- nothing to do"
        return
    fi
    ORIG[governor]="$(cat "$base/scaling_governor")"
    if ! grep -qw performance "$base/scaling_available_governors" 2>/dev/null; then
        skip "CPU governor: 'performance' not in scaling_available_governors, leaving as-is"
        return
    fi
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$f" 2>/dev/null
    done
    local bad=""
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ "$(cat "$f")" = "performance" ] || bad="$bad $f"
    done
    if [ -z "$bad" ]; then
        ok "CPU governor: performance on all CPUs (was: ${ORIG[governor]})"
    else
        fail "CPU governor: did not take effect on:$bad"
    fi
}
restore_governor() {
    [ -n "${ORIG_governor:-}" ] || return
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo "$ORIG_governor" > "$f" 2>/dev/null
    done
    local bad=""
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ "$(cat "$f")" = "$ORIG_governor" ] || bad="$bad $f"
    done
    if [ -z "$bad" ]; then
        ok "CPU governor restored: $ORIG_governor"
    else
        fail "CPU governor: restore to '$ORIG_governor' did not take effect on:$bad"
    fi
}

apply_turbo() {
    # Two mutually exclusive sysfs interfaces depending on which cpufreq
    # driver is active; only one will exist on a given machine.
    if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
        ORIG[turbo_boost]="$(cat /sys/devices/system/cpu/cpufreq/boost)"
        echo 0 > /sys/devices/system/cpu/cpufreq/boost 2>/dev/null
        if [ "$(cat /sys/devices/system/cpu/cpufreq/boost)" = "0" ]; then
            ok "Turbo Boost off (boost=0, was: ${ORIG[turbo_boost]})"
        else
            fail "Turbo Boost: wrote 0 to boost but it did not take effect"
        fi
    elif [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        ORIG[turbo_no_turbo]="$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)"
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
        if [ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)" = "1" ]; then
            ok "Turbo Boost off (intel_pstate/no_turbo=1, was: ${ORIG[turbo_no_turbo]})"
        else
            fail "Turbo Boost: wrote 1 to no_turbo but it did not take effect"
        fi
    else
        skip "Turbo Boost: no known sysfs control found (virtualized CPU? not exposed here)"
    fi
}
restore_turbo() {
    if [ -n "${ORIG_turbo_boost:-}" ]; then
        echo "$ORIG_turbo_boost" > /sys/devices/system/cpu/cpufreq/boost 2>/dev/null
        if [ "$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)" = "$ORIG_turbo_boost" ]; then
            ok "Turbo Boost restored: boost=$ORIG_turbo_boost"
        else
            fail "Turbo Boost: restore of boost=$ORIG_turbo_boost did not take effect"
        fi
    elif [ -n "${ORIG_turbo_no_turbo:-}" ]; then
        echo "$ORIG_turbo_no_turbo" > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
        if [ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null)" = "$ORIG_turbo_no_turbo" ]; then
            ok "Turbo Boost restored: no_turbo=$ORIG_turbo_no_turbo"
        else
            fail "Turbo Boost: restore of no_turbo=$ORIG_turbo_no_turbo did not take effect"
        fi
    fi
}

apply_smt() {
    local f=/sys/devices/system/cpu/smt/control
    if [ ! -f "$f" ]; then
        skip "SMT control: not exposed on this kernel/CPU"
        return
    fi
    ORIG[smt]="$(cat "$f")"
    if [ "${ORIG[smt]}" = "notsupported" ] || [ "${ORIG[smt]}" = "notimplemented" ]; then
        skip "SMT control: CPU reports '${ORIG[smt]}' -- nothing to toggle"
        return
    fi
    if ! echo off > "$f" 2>/tmp/tune_smt_err; then
        fail "SMT: write to $f failed ($(cat /tmp/tune_smt_err 2>/dev/null)) -- SMT is still '${ORIG[smt]}'. Likely a virtualized/hot-unpluggable-CPU restriction; on bare metal this should not happen."
        rm -f /tmp/tune_smt_err
        return
    fi
    rm -f /tmp/tune_smt_err
    local now
    now="$(cat "$f")"
    if [ "$now" = "off" ] || [ "$now" = "forceoff" ]; then
        ok "SMT (hyperthreading) off (was: ${ORIG[smt]})"
    else
        fail "SMT: wrote 'off' to $f but it now reads '$now', not off/forceoff -- treat SMT as still ON"
    fi
}
restore_smt() {
    [ -n "${ORIG_smt:-}" ] || return
    [ "$ORIG_smt" = "notsupported" ] || [ "$ORIG_smt" = "notimplemented" ] && return
    echo "$ORIG_smt" > /sys/devices/system/cpu/smt/control 2>/dev/null
    local now
    now="$(cat /sys/devices/system/cpu/smt/control 2>/dev/null)"
    if [ "$now" = "$ORIG_smt" ]; then
        ok "SMT restored: $ORIG_smt"
    else
        fail "SMT: tried to restore '$ORIG_smt' but it now reads '$now' -- verify manually"
    fi
}

apply_aslr() {
    ORIG[aslr]="$(sysctl -n kernel.randomize_va_space)"
    sysctl -qw kernel.randomize_va_space=0
    local now
    now="$(sysctl -n kernel.randomize_va_space)"
    if [ "$now" = "0" ]; then
        ok "ASLR off (kernel.randomize_va_space=0, was: ${ORIG[aslr]})"
    else
        fail "ASLR: wrote 0 but kernel.randomize_va_space now reads '$now'"
    fi
}
restore_aslr() {
    [ -n "${ORIG_aslr:-}" ] || return
    sysctl -qw "kernel.randomize_va_space=$ORIG_aslr"
    local now
    now="$(sysctl -n kernel.randomize_va_space)"
    if [ "$now" = "$ORIG_aslr" ]; then
        ok "ASLR restored: $ORIG_aslr"
    else
        fail "ASLR: tried to restore $ORIG_aslr but it now reads '$now'"
    fi
}

apply_nmi_watchdog() {
    ORIG[nmi]="$(sysctl -n kernel.nmi_watchdog)"
    sysctl -qw kernel.nmi_watchdog=0
    local now
    now="$(sysctl -n kernel.nmi_watchdog)"
    if [ "$now" = "0" ]; then
        ok "NMI watchdog off (was: ${ORIG[nmi]})"
    else
        fail "NMI watchdog: wrote 0 but kernel.nmi_watchdog now reads '$now'"
    fi
}
restore_nmi_watchdog() {
    [ -n "${ORIG_nmi:-}" ] || return
    sysctl -qw "kernel.nmi_watchdog=$ORIG_nmi"
    local now
    now="$(sysctl -n kernel.nmi_watchdog)"
    if [ "$now" = "$ORIG_nmi" ]; then
        ok "NMI watchdog restored: $ORIG_nmi"
    else
        fail "NMI watchdog: tried to restore $ORIG_nmi but it now reads '$now'"
    fi
}

apply_thp() {
    local f=/sys/kernel/mm/transparent_hugepage/enabled
    if [ ! -f "$f" ]; then
        skip "THP: not exposed on this kernel"
        return
    fi
    ORIG[thp]="$(grep -o '\[[a-z]*\]' "$f" | tr -d '[]')"
    echo never > "$f" 2>/dev/null
    local now
    now="$(grep -o '\[[a-z]*\]' "$f" | tr -d '[]')"
    if [ "$now" = "never" ]; then
        ok "Transparent Huge Pages off (was: ${ORIG[thp]})"
    else
        fail "THP: wrote 'never' but $f now reports current='$now'"
    fi
}
restore_thp() {
    [ -n "${ORIG_thp:-}" ] || return
    local f=/sys/kernel/mm/transparent_hugepage/enabled
    echo "$ORIG_thp" > "$f" 2>/dev/null
    local now
    now="$(grep -o '\[[a-z]*\]' "$f" | tr -d '[]')"
    if [ "$now" = "$ORIG_thp" ]; then
        ok "THP restored: $ORIG_thp"
    else
        fail "THP: tried to restore '$ORIG_thp' but $f now reports current='$now'"
    fi
}

apply_swap() {
    local before
    before="$(tail -n +2 /proc/swaps 2>/dev/null)"
    if [ -z "$before" ]; then
        skip "Swap: already off"
        return
    fi
    # Remember exactly which swap devices were active, so restore can bring
    # back those specific ones (swapon -a re-reads /etc/fstab, which may not
    # list every currently-active swap device, e.g. one turned on by hand).
    ORIG[swap_devices]="$(echo "$before" | awk '{print $1}' | tr '\n' ' ' | sed 's/ $//' | tr ' ' ',')"
    swapoff -a 2>/tmp/tune_swap_err
    local now
    now="$(tail -n +2 /proc/swaps 2>/dev/null)"
    if [ -z "$now" ]; then
        ok "Swap off (was: ${ORIG[swap_devices]} -- note: this does not touch" \
           "/etc/fstab, so a REBOOT would bring it back; restore turns it back on)"
    else
        fail "Swap: swapoff -a did not fully turn it off ($(cat /tmp/tune_swap_err 2>/dev/null)); still active: $(echo "$now" | awk '{print $1}' | tr '\n' ' ')"
    fi
    rm -f /tmp/tune_swap_err
}
restore_swap() {
    [ -n "${ORIG_swap_devices:-}" ] || return
    swapon -a 2>/dev/null
    IFS=',' read -ra devs <<< "$ORIG_swap_devices"
    for d in "${devs[@]}"; do
        grep -q "^$d " /proc/swaps 2>/dev/null || swapon "$d" 2>/dev/null
    done
    local now missing=""
    now="$(tail -n +2 /proc/swaps 2>/dev/null | awk '{print $1}')"
    for d in "${devs[@]}"; do
        echo "$now" | grep -qx "$d" || missing="$missing $d"
    done
    if [ -z "$missing" ]; then
        ok "Swap restored: $ORIG_swap_devices"
    else
        fail "Swap: could not re-enable:$missing -- check manually (swapon <device>); on WSL2 this can" \
             "fail for reasons unrelated to this script (the swap device may be managed by the Windows" \
             "host), but on bare metal it should just work"
    fi
}

apply_clocksource() {
    local f=/sys/devices/system/clocksource/clocksource0/current_clocksource
    local avail=/sys/devices/system/clocksource/clocksource0/available_clocksource
    if [ ! -f "$f" ]; then
        skip "Clocksource: not exposed on this kernel"
        return
    fi
    ORIG[clocksource]="$(cat "$f")"
    if grep -qw tsc "$avail" 2>/dev/null; then
        echo tsc > "$f" 2>/dev/null
        if [ "$(cat "$f")" = "tsc" ]; then
            ok "Clocksource: tsc (was: ${ORIG[clocksource]})"
        else
            fail "Clocksource: wrote tsc but it did not take effect (still: $(cat "$f"))"
        fi
    else
        note "Clocksource: tsc not available on this machine (available: $(cat "$avail" 2>/dev/null))," \
             "leaving as '${ORIG[clocksource]}'"
    fi
}
restore_clocksource() {
    [ -n "${ORIG_clocksource:-}" ] || return
    local f=/sys/devices/system/clocksource/clocksource0/current_clocksource
    echo "$ORIG_clocksource" > "$f" 2>/dev/null
    if [ "$(cat "$f" 2>/dev/null)" = "$ORIG_clocksource" ]; then
        ok "Clocksource restored: $ORIG_clocksource"
    else
        fail "Clocksource: restore to '$ORIG_clocksource' did not take effect"
    fi
}

# =========================
# status: report current values, change nothing
# =========================
show_status() {
    log "Current machine tuning state (read-only, nothing changed):"
    local base=/sys/devices/system/cpu/cpu0/cpufreq
    [ -d "$base" ] && note "CPU governor: $(cat "$base/scaling_governor")" || note "CPU governor: n/a (no cpufreq sysfs)"
    if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
        note "Turbo Boost: boost=$(cat /sys/devices/system/cpu/cpufreq/boost)"
    elif [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        note "Turbo Boost: no_turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)"
    else
        note "Turbo Boost: n/a"
    fi
    [ -f /sys/devices/system/cpu/smt/control ] && note "SMT: $(cat /sys/devices/system/cpu/smt/control)" || note "SMT: n/a"
    note "ASLR (kernel.randomize_va_space): $(sysctl -n kernel.randomize_va_space)"
    note "NMI watchdog (kernel.nmi_watchdog): $(sysctl -n kernel.nmi_watchdog)"
    [ -f /sys/kernel/mm/transparent_hugepage/enabled ] && note "THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled)" || note "THP: n/a"
    note "Swap: $([ -z "$(tail -n +2 /proc/swaps 2>/dev/null)" ] && echo off || echo on)"
    [ -f /sys/devices/system/clocksource/clocksource0/current_clocksource ] && \
        note "Clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)" || note "Clocksource: n/a"
    if [ -f "$STATE_FILE" ]; then
        note "Saved state file present at $STATE_FILE (apply has run, restore is available)"
    fi
    return 0
}

main() {
    [ "$MODE" != "status" ] && need_root
    log "Repository root: $ROOT_DIR"

    case "$MODE" in
      apply)
        if [ -f "$STATE_FILE" ]; then
            echo "A state file already exists at $STATE_FILE -- apply has already run."
            echo "Run './tune_machine.sh restore' first if you want to re-apply from a clean state,"
            echo "or './tune_machine.sh status' to see current values."
            exit 1
        fi
        log "Applying machine tuning..."
        apply_governor
        apply_turbo
        apply_smt
        apply_aslr
        apply_nmi_watchdog
        apply_thp
        apply_swap
        apply_clocksource
        state_save
        echo
        if [ "$FAIL_COUNT" -eq 0 ]; then
            log "Done, no failures. State saved to $STATE_FILE -- run './tune_machine.sh restore' to undo everything."
        else
            log "Done, but $FAIL_COUNT tunable(s) FAILED to apply (see FAIL lines above)."
            echo "State was still saved (whatever DID apply is recorded) -- fix the reported"
            echo "issue if you can, otherwise proceed knowing those specific settings are"
            echo "still at their original values. Do not assume everything above applied"
            echo "just because the script reached the end."
            exit 1
        fi
        ;;
      restore)
        log "Restoring original machine state..."
        state_load
        restore_governor
        restore_turbo
        restore_smt
        restore_aslr
        restore_nmi_watchdog
        restore_thp
        restore_swap
        restore_clocksource
        rm -f "$STATE_FILE"
        if [ "$FAIL_COUNT" -eq 0 ]; then
            log "Done, no failures. Machine restored to its pre-tuning state."
        else
            log "Done, but $FAIL_COUNT tunable(s) FAILED to restore cleanly (see FAIL lines above)."
            echo "The state file was still removed. Check the machine's actual state with"
            echo "'./tune_machine.sh status' and fix anything listed above by hand."
            exit 1
        fi
        ;;
      status)
        show_status
        ;;
      *)
        echo "Usage: $0 [apply|restore|status]"
        exit 1
        ;;
    esac
}

main "$@"
