"""Scenario definitions shared by every implementation's run_bench.py.

Four scenarios (each its own server/bench pair, grid of cases/threads/
repetitions) matrix: the SCENARIO. All four run in a single `run.sh` invocation;
nothing to toggle by hand.

  streaming     E0  the published model: the client reads the byte-stream as it
                    arrives (read loop, no whole-object materialisation). Wire is
                    raw-until-close; the server is `tcpserver`.
  whole_object  E1  the client receives the transfer as ONE object, materialised
                    contiguously. Non-TAPS: accumulate into one growing buffer.
                    TAPS: PassthroughFramer(gather=true) -> one receive() ->
                    as_bytes().
  framed        E3  length-prefixed application framing, NO security layer --
                    the plaintext mirror of "tls_framed" (same manifest, same
                    frame format, same deframing loop, just no TLS record
                    layer), so framing cost can be measured on its own instead
                    of only ever bundled with TLS's cost. Replaces an earlier
                    "blocks" scenario that tried to measure segment-by-segment
                    Message consumption via taps::PassthroughFramer(gather=false)
                    -- that framer can only ever emit after the connection's
                    half-close (no boundary marker exists on a raw-until-close
                    wire for it to key off), so it was structurally incapable
                    of the incremental delivery its description implied.
                    taps::LengthPrefixedFramer does real incremental delivery
                    (its parse() ignores at_eof and emits the instant a full
                    record has arrived), already proven correct by
                    "tls_framed"'s real-hardware data -- this scenario is that
                    same design with TLS removed. Non-TAPS: the shared
                    tls/frame_reader.hpp deframing loop (TLS-agnostic despite
                    the directory). TAPS: LengthPrefixedFramer, same as
                    "tls_framed".
  udp_k64       E4  the same transfer over UDP, max-size IPv4 datagrams
                    (65507 bytes -- true limit, "64 KiB" literally overflows
                    it), 5-way.
  udp_k1400     E4  same, ~MTU-sized datagrams.

Same wire for streaming / whole_object (raw-until-close), so they share
`tcpserver`. "framed" has its own wire (length-prefixed messages, no security)
and its own server, `tcpserver_framed`, mirroring "tls_framed"'s
`tcpserver_tls_framed` minus the TLS record layer. Per-scenario grid / target
binaries / env live in SCENARIOS. Output and resume-state are per scenario:
results/<scenario>/... so a crashed campaign resumes and `run.sh` aggregates
each scenario independently.

Env:
  RUN_SCENARIOS="framed udp"   run only these (space/comma separated); default all
  DRY_RUN=1                    plumbing-only local validation, no real execution
"""

import json
import os
from pathlib import Path

# name -> {server, bench, cases, threads, env}
#   server / bench: basename under build-<compiler>/<server>/<server> and
#                   build-<compiler>/benchmarks/<bench>
#   env:            extra environment for both the server and the bench processes
_TCP_GRID = dict(cases=[1, 2, 4, 8, 16], threads=[1, 2, 4, 8])
_UDP_GRID = dict(cases=[1, 2, 4, 8], threads=[1, 2, 4])

# The tls / tls_framed scenarios add a TLS 1.3 record layer to the streaming and
# framed models. Cert/CA paths are relative to each subproject dir (run_bench.py's
# cwd). The pinned TLS parameters live in tls/tls_common.hpp; every arm's server
# and client print a TLS_IDENTITY line the runner checks for equality.
_TLS_ENV = {"TLS_CERT": "../tls/server.crt",
            "TLS_KEY":  "../tls/server.key",
            "TLS_CA":   "../tls/ca.crt"}

SCENARIOS = {
    "streaming":    dict(server="tcpserver", bench="bench_tcp",        **_TCP_GRID),
    "whole_object": dict(server="tcpserver", bench="bench_tcp_whole",  **_TCP_GRID),
    "framed":       dict(server="tcpserver_framed", bench="bench_tcp_framed",
                         env={"MANIFEST": "../tls/manifest.txt"}, **_TCP_GRID),
    "tls":          dict(server="tcpserver_tls", bench="bench_tcp_tls",
                         env=dict(_TLS_ENV), **_TCP_GRID),
    "tls_framed":   dict(server="tcpserver_tls_framed", bench="bench_tcp_tls_framed",
                         env={**_TLS_ENV, "TLS_MANIFEST": "../tls/manifest.txt"},
                         **_TCP_GRID),
    # 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
    # payload. 65536 ("64 KiB" literally) is one byte over it and every
    # sendto() of that size fails with EMSGSIZE -- confirmed against this box.
    "udp_k64":      dict(server="udpserver", bench="bench_udp", env={"DGRAM_BYTES": "65507"}, **_UDP_GRID),
    "udp_k1400":    dict(server="udpserver", bench="bench_udp", env={"DGRAM_BYTES": "1400"},  **_UDP_GRID),
}

DRY_RUN = os.environ.get("DRY_RUN", "").strip().lower() not in ("", "0", "false", "no")


def active_scenarios():
    raw = os.environ.get("RUN_SCENARIOS", "").strip()
    if not raw:
        return list(SCENARIOS.keys())
    names = [n for chunk in raw.replace(",", " ").split() for n in [chunk] if n]
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise SystemExit(f"Unknown scenario(s) in RUN_SCENARIOS: {unknown}. "
                          f"Valid: {list(SCENARIOS.keys())}")
    return names


def checkpoint_path(results_dir: str) -> Path:
    return Path(results_dir) / "checkpoints" / "scenario_done.json"


def is_done(results_dir: str) -> bool:
    p = checkpoint_path(results_dir)
    return p.exists() and json.loads(p.read_text()).get("done") is True


def load_done(results_dir: str) -> dict:
    p = checkpoint_path(results_dir)
    if not p.exists():
        return {}
    return json.loads(p.read_text())


def mark_done(results_dir: str, **extra) -> None:
    p = checkpoint_path(results_dir)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps({"done": True, **extra}, indent=2))
