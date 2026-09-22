#!/usr/bin/env bash
# Shared functions for the D7 network-realism experiment: a Linux network
# namespace + veth pair carries all client<->server traffic, with `tc netem`
# shaping (delay + bandwidth cap + Gilbert loss) applied independently on each
# veth endpoint. See design/receive_path_redesign.md D7 and
# design/tls_experiment_notes.md for the reviewer question (R2-c) this answers,
# and the 2026-09-14 session notes there for why this replaced an earlier
# `lo`-only version.
#
# Why netns+veth instead of shaping `lo` directly:
#   `lo` is a single shared egress queue for both directions of every flow on
#   the machine -- a delay set there is not attributable to "the link", it is
#   shared by ANY loopback traffic, and it cannot be shaped asymmetrically per
#   direction. A veth pair gives two genuinely separate interfaces (one queue
#   each), which is the standard technique for a reproducible, single-machine
#   "impaired remote host" testbed (see e.g. the container/netns-based
#   emulation testbeds cited in the design notes). The server process runs
#   inside NETNS_NAME; the client (and the rest of this harness) stays in the
#   root namespace, connecting to the namespace's veth IP instead of 127.0.0.1.
#
# Why a rate cap is mandatory, not optional (discovered 2026-09-14, WSL2 pilot
#   against the earlier `lo`-only version, same physics applies here):
#   `netem delay Xms` alone, with no bandwidth cap, does not bound the
#   bandwidth-delay product. A veth pair's bandwidth is effectively unbounded
#   (same as loopback), so TCP's congestion window keeps growing until it
#   overflows netem's internal queue (default limit: 1000 packets). For a
#   100 MB bulk transfer that overflow is the default outcome, not a corner
#   case: the queue drops packets, TCP recovers via retransmission timeout
#   (RTO, hundreds of ms on Linux), and single benchmark iterations can stall
#   for tens of seconds. A `rate` cap turns "delay + unbounded bandwidth" into
#   "delay + bounded bandwidth" -- both a correctness fix (bounds the queue to
#   a realistic bandwidth-delay product) and closer to what R2-c actually asks
#   for (a real link has both latency AND bandwidth).
#
# Round-trip calibration: delay is applied once per veth endpoint's egress.
# A round trip crosses both endpoints once each (request out through
# veth-host, reply out through veth-peer) -- so setting delay=X on each side
# gives a round-trip time of ~2X, same factor-of-2 as the earlier `lo`
# version (confirmed there empirically with `ping`; the arithmetic is
# unchanged, only the topology is). netem_set_link() takes the TARGET
# round-trip time and halves it internally.
#
# Loss model: Simple Gilbert (tc-netem's `loss gemodel p r`, the 2-parameter
# case -- 100% loss in the "bad" state, 0% in the "good" state), not
# independent/Bernoulli loss. Real network loss is bursty, not independent
# per packet (Gilbert, "Capacity of a Burst-Noise Channel", Bell System
# Technical Journal 39(5), 1960; Elliott, "Estimates of Error Rates for Codes
# on Burst-Noise Channels", BSTJ 42(5), 1963 -- the model tc-netem's gemodel
# implements). Parameterised here by the two quantities a reader actually
# cares about instead of the raw Markov transition probabilities:
#   target average loss rate L, and mean burst length B (packets).
# Steady-state of the 2-state chain: P(bad) = p/(p+r) = L (all loss happens in
# the bad state); mean sojourn in the bad state = 1/r = B packets. Solving:
#   r = 1/B          p = L*r/(1-L)
# netem_set_link() takes L and B and does this conversion so nobody has to
# hand-pick p/r directly.

NETNS_NAME="${NETNS_NAME:-netlab-srv}"
VETH_HOST="${VETH_HOST:-veth-host}"
VETH_PEER="${VETH_PEER:-veth-peer}"
VETH_HOST_IP="${VETH_HOST_IP:-10.200.1.1}"
VETH_PEER_IP="${VETH_PEER_IP:-10.200.1.2}"
VETH_PREFIX="${VETH_PREFIX:-30}"   # /30: exactly the 2 addresses above, standard for a point-to-point link

NETEM_RATE_MBIT="${NETEM_RATE_MBIT:-1000}"    # bandwidth cap paired with the delay; default 1 Gbit/s
NETEM_LIMIT_PKTS="${NETEM_LIMIT_PKTS:-50000}" # queue depth; generous headroom over the BDP at the rate/RTT above
NETEM_MEAN_BURST_PKTS="${NETEM_MEAN_BURST_PKTS:-3}" # Simple Gilbert: mean consecutive packets lost per loss event

# Exported so run_bench.py (all 5 projects) and this script's own ping-based
# verification target the server's namespace IP instead of 127.0.0.1.
export NETEM_SERVER_HOST="$VETH_PEER_IP"
export NETEM_SERVER_NETNS="$NETNS_NAME"

_netem_log() {
    printf '[netem] %s\n' "$*"
}

_netem_run_privileged() {
    if [ "${EUID:-$(id -u)}" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo -n "$@" 2>/dev/null || sudo "$@"
    else
        echo "[netem] Error: root privileges required to run: $*" >&2
        echo "[netem] Run this script with sudo, or run build.sh once first" \
             "(it installs a NOPASSWD sudo rule scoped to tc and ip)." >&2
        exit 1
    fi
}

_netns_exec() {
    _netem_run_privileged ip netns exec "$NETNS_NAME" "$@"
}

# Fails loudly and early instead of silently measuring an unshaped link and
# mislabelling results with an RTT/loss rate that was never really applied.
netem_require() {
    for cmd in tc ip; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "[netem] Error: '$cmd' (iproute2) not found. Run build.sh first." >&2
            exit 1
        fi
    done
    if ! _netem_run_privileged ip netns list >/dev/null 2>&1; then
        echo "[netem] Error: cannot manage network namespaces even with sudo." >&2
        echo "[netem] Run build.sh first (it sets up passwordless sudo for tc/ip)," \
             "or run this script itself under sudo." >&2
        exit 1
    fi
}

# Idempotent: safe to call at the start of every sweep even if a previous run
# left the topology up (e.g. after a crash before netns_teardown ran).
netns_setup() {
    if _netem_run_privileged ip netns list 2>/dev/null | grep -q "^${NETNS_NAME}\b"; then
        _netem_log "Namespace $NETNS_NAME already exists, reusing it"
        return
    fi

    _netem_log "Creating namespace $NETNS_NAME and veth pair $VETH_HOST <-> $VETH_PEER"
    _netem_run_privileged ip netns add "$NETNS_NAME"
    _netem_run_privileged ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
    _netem_run_privileged ip link set "$VETH_PEER" netns "$NETNS_NAME"

    _netem_run_privileged ip addr add "${VETH_HOST_IP}/${VETH_PREFIX}" dev "$VETH_HOST"
    _netem_run_privileged ip link set "$VETH_HOST" up

    _netns_exec ip addr add "${VETH_PEER_IP}/${VETH_PREFIX}" dev "$VETH_PEER"
    _netns_exec ip link set "$VETH_PEER" up
    _netns_exec ip link set lo up

    _netem_log "Topology up: root ns $VETH_HOST=$VETH_HOST_IP <-> $NETNS_NAME $VETH_PEER=$VETH_PEER_IP"
}

# Deleting the namespace also removes veth-peer; the veth pair as a whole
# (both ends) is torn down automatically once either end is deleted.
netns_teardown() {
    if _netem_run_privileged ip netns list 2>/dev/null | grep -q "^${NETNS_NAME}\b"; then
        _netem_log "Deleting namespace $NETNS_NAME (and its end of the veth pair)"
        _netem_run_privileged ip netns del "$NETNS_NAME"
    fi
    # Belt and suspenders: if the namespace delete somehow left veth-host
    # dangling (e.g. it was never moved), remove it explicitly too.
    if _netem_run_privileged ip link show "$VETH_HOST" >/dev/null 2>&1; then
        _netem_run_privileged ip link del "$VETH_HOST" 2>/dev/null || true
    fi
}

_gilbert_pr() {
    # Prints "P R" (percentages, 4 decimals) for a target loss rate (0-100)
    # and mean burst length in packets. See the file header for the derivation.
    local loss_pct="$1" mean_burst_pkts="$2"
    python3 -c "
L = float('$loss_pct') / 100.0
B = float('$mean_burst_pkts')
r = 1.0 / B
p = L * r / (1.0 - L)
print(f'{p*100:.4f} {r*100:.4f}')
"
}

# netem_set_link <target_rtt_ms> <target_loss_pct> [mean_burst_pkts]
# Either argument may be 0. Applies identical (delay, loss) shaping to BOTH
# veth endpoints (symmetric link assumption), each contributing half the RTT.
netem_set_link() {
    local rtt_ms="$1" loss_pct="${2:-0}" mean_burst_pkts="${3:-$NETEM_MEAN_BURST_PKTS}"
    netem_clear_link

    if [ "$rtt_ms" = "0" ] && [ "$loss_pct" = "0" ]; then
        _netem_log "RTT=0, loss=0: leaving $VETH_HOST/$VETH_PEER unshaped (baseline)"
        return
    fi

    local one_way="0"
    if [ "$rtt_ms" != "0" ]; then
        one_way="$(python3 -c "print($rtt_ms/2)")"
    fi

    local loss_clause=""
    if [ "$loss_pct" != "0" ]; then
        local pr
        pr="$(_gilbert_pr "$loss_pct" "$mean_burst_pkts")"
        local p r
        p="$(echo "$pr" | awk '{print $1}')"
        r="$(echo "$pr" | awk '{print $2}')"
        loss_clause="loss gemodel ${p}% ${r}%"
        _netem_log "Loss: target=${loss_pct}% mean_burst=${mean_burst_pkts}pkt -> Simple Gilbert p=${p}% r=${r}%"
    fi

    _netem_log "Shaping $VETH_HOST (root ns): delay=${one_way}ms rate=${NETEM_RATE_MBIT}mbit limit=${NETEM_LIMIT_PKTS} ${loss_clause}"
    # shellcheck disable=SC2086
    _netem_run_privileged tc qdisc add dev "$VETH_HOST" root netem \
        delay "${one_way}ms" rate "${NETEM_RATE_MBIT}mbit" limit "$NETEM_LIMIT_PKTS" $loss_clause

    _netem_log "Shaping $VETH_PEER ($NETNS_NAME): delay=${one_way}ms rate=${NETEM_RATE_MBIT}mbit limit=${NETEM_LIMIT_PKTS} ${loss_clause}"
    # shellcheck disable=SC2086
    _netns_exec tc qdisc add dev "$VETH_PEER" root netem \
        delay "${one_way}ms" rate "${NETEM_RATE_MBIT}mbit" limit "$NETEM_LIMIT_PKTS" $loss_clause
}

netem_clear_link() {
    if _netem_run_privileged ip link show "$VETH_HOST" >/dev/null 2>&1; then
        if _netem_run_privileged tc qdisc show dev "$VETH_HOST" 2>/dev/null | grep -q netem; then
            _netem_run_privileged tc qdisc del dev "$VETH_HOST" root
        fi
    fi
    if _netem_run_privileged ip netns list 2>/dev/null | grep -q "^${NETNS_NAME}\b"; then
        if _netns_exec tc qdisc show dev "$VETH_PEER" 2>/dev/null | grep -q netem; then
            _netns_exec tc qdisc del dev "$VETH_PEER" root
        fi
    fi
}

# netem_verify_rtt <target_rtt_ms> <tolerance_ms>
# Pings across the veth link (root ns -> namespace IP), not loopback.
netem_verify_rtt() {
    local target_rtt_ms="$1" tolerance_ms="${2:-2}"
    local measured attempt

    # 20 samples (not 5) so one transient blip (ARP resolution right after
    # the veth pair is created, a momentary scheduling hiccup) can't swing
    # the average past a tight tolerance on its own -- confirmed 2026-09-22:
    # a single +50ms outlier among 5 pings shifted the average by +10ms and
    # wrongly aborted an otherwise correctly-shaped RTT=50ms link. One retry
    # on top, since a genuinely mis-shaped link will fail again but a
    # one-off transient won't.
    for attempt in 1 2; do
        measured="$(ping -c 20 -q "$VETH_PEER_IP" 2>/dev/null | grep rtt | awk -F'/' '{print $5}')"
        if [ -z "$measured" ]; then
            echo "[netem] Error: could not measure RTT with ping to $VETH_PEER_IP" >&2
            echo "[netem] Is the namespace topology up? (netns_setup)" >&2
            exit 1
        fi
        _netem_log "Requested RTT=${target_rtt_ms}ms, measured (ping avg of 20 to $VETH_PEER_IP, attempt $attempt/2)=${measured}ms"
        if python3 -c "
import sys
target, measured, tol = float('$target_rtt_ms'), float('$measured'), float('$tolerance_ms')
sys.exit(0 if abs(measured - target) <= tol else 1)
"; then
            return 0
        fi
        [ "$attempt" -eq 1 ] && sleep 2
    done

    echo "[netem] Error: measured RTT (${measured}ms) is more than ${tolerance_ms}ms away from the target (${target_rtt_ms}ms), even after a retry." >&2
    echo "[netem] Aborting this sweep point rather than recording a mislabelled result." >&2
    exit 1
}
