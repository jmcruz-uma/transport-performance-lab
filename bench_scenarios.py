"""Scenario definitions shared by every implementation's run_bench.py.

Four scenarios (each its own server/bench pair, grid of cases/threads/
repetitions) matrix: the SCENARIO. All four run in a single `run.sh` invocation;
nothing to toggle by hand.

  streaming     E0  the published model: the client reads the byte-stream as it
                    arrives (read loop, the transfer is never held whole). Wire
                    is raw-until-close; the server is `tcpserver`.
  whole_transfer
                E1  the client receives the whole transfer as ONE contiguous
                    buffer. Non-TAPS: accumulate into one growing buffer.
                    TAPS: PassthroughFramer(gather=true) -> one receive() ->
                    as_bytes().
  streaming_naive, whole_transfer_naive
                E8  TAPS only: the same clients as streaming / whole_transfer,
                    with message memory taken from the heap and never recycled
                    (std::pmr::new_delete_resource) instead of the library's
                    default recycling pool. Other arms skip them (no binaries).
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
  <scenario>_crc    for streaming, whole_transfer, framed, tls, tls_framed, udp_k1400,
                    streaming_naive and whole_transfer_naive: the same server and
                    clients built with CONSUME_CRC (tls/consume_crc.hpp), which run a
                    CRC-32C over every byte of each complete unit received (the
                    clients without the suffix discard the data). The TCP clients
                    check the final CRC against EXPECTED_CRC32C, computed here by
                    tls/crc32c_expected (built by build.sh) over the bytes the server
                    sends; UDP may lose datagrams, so it only processes them.

Same wire for streaming / whole_transfer and their naive variants
(raw-until-close), so they share `tcpserver`. "framed" has its own wire (length-prefixed messages, no security)
and its own server, `tcpserver_framed`, mirroring "tls_framed"'s
`tcpserver_tls_framed` minus the TLS record layer. Per-scenario grid / target
binaries / env live in SCENARIOS. Output and resume-state are per scenario:
results/<scenario>/... so a crashed campaign resumes and `run.sh` aggregates
each scenario independently.

Env:
  RUN_SCENARIOS="framed udp"   run only these (space/comma separated); default all
                               but the *_crc ones, which run only when named;
                               "crc" names all of them
  DRY_RUN=1                    plumbing-only local validation, no real execution
  PAYLOAD=10MB                 payload of every scenario: 100MB (default) or 10MB
  TCP_CASES, TCP_THREADS,      client counts and server thread counts of the TCP
  UDP_CASES, UDP_THREADS       and UDP scenarios (space separated); default the
                               full grid below. The D7 sweep narrows them.
"""

import json
import os
import subprocess
from pathlib import Path

# name -> {server, bench, cases, threads, env}
#   server / bench: basename under build-<compiler>/<server>/<server> and
#                   build-<compiler>/benchmarks/<bench>
#   env:            extra environment for both the server and the bench processes
# Full grid by default. The D7 sweep (netem/run_rtt_sweep.sh) narrows it through the
# environment to fit its time budget.
def _grid(cases_var, cases, threads_var, threads):
    return dict(cases=[int(c) for c in os.environ.get(cases_var, cases).split()],
                threads=[int(t) for t in os.environ.get(threads_var, threads).split()])

_TCP_GRID = _grid("TCP_CASES", "1 2 4 8 16", "TCP_THREADS", "1 2 4 8")
_UDP_GRID = _grid("UDP_CASES", "1 2 4 8", "UDP_THREADS", "1 2 4")

# The payload every server sends and, for the framed scenarios, the manifest of message
# sizes cut from it. Chosen together so the two can never disagree. Paths are relative
# to each subproject dir (run_bench.py's cwd).
_PAYLOADS = {
    "100MB": ("../files/100MB.bin", "../tls/manifest.txt"),
    "10MB":  ("../files/10MB.bin",  "../tls/manifest_10MB.txt"),
}
PAYLOAD = os.environ.get("PAYLOAD", "100MB")
if PAYLOAD not in _PAYLOADS:
    raise SystemExit(f"Unknown PAYLOAD={PAYLOAD!r}. Valid: {list(_PAYLOADS)}")
PAYLOAD_FILE, _MANIFEST = _PAYLOADS[PAYLOAD]

# The tls / tls_framed scenarios add a TLS 1.3 record layer to the streaming and
# framed models. Cert/CA paths are relative to each subproject dir (run_bench.py's
# cwd). The pinned TLS parameters live in tls/tls_common.hpp; every arm's server
# and client print a TLS_IDENTITY line the runner checks for equality.
_TLS_ENV = {"TLS_CERT": "../tls/server.crt",
            "TLS_KEY":  "../tls/server.key",
            "TLS_CA":   "../tls/ca.crt"}

SCENARIOS = {
    "streaming":    dict(server="tcpserver", bench="bench_tcp",        **_TCP_GRID),
    "whole_transfer": dict(server="tcpserver", bench="bench_tcp_whole_transfer",  **_TCP_GRID),
    "streaming_naive":      dict(server="tcpserver", bench="bench_tcp_naive",                **_TCP_GRID),
    "whole_transfer_naive": dict(server="tcpserver", bench="bench_tcp_whole_transfer_naive", **_TCP_GRID),
    "framed":       dict(server="tcpserver_framed", bench="bench_tcp_framed",
                         env={"MANIFEST": _MANIFEST}, **_TCP_GRID),
    "tls":          dict(server="tcpserver_tls", bench="bench_tcp_tls",
                         env=dict(_TLS_ENV), **_TCP_GRID),
    "tls_framed":   dict(server="tcpserver_tls_framed", bench="bench_tcp_tls_framed",
                         env={**_TLS_ENV, "TLS_MANIFEST": _MANIFEST},
                         **_TCP_GRID),
    # 65507 = 65535 - 8 (UDP header) - 20 (IPv4 header): the true max IPv4 UDP
    # payload. 65536 ("64 KiB" literally) is one byte over it and every
    # sendto() of that size fails with EMSGSIZE -- confirmed against this box.
    "udp_k64":      dict(server="udpserver", bench="bench_udp", env={"DGRAM_BYTES": "65507"}, **_UDP_GRID),
    "udp_k1400":    dict(server="udpserver", bench="bench_udp", env={"DGRAM_BYTES": "1400"},  **_UDP_GRID),
}

# "_crc" variants: same server, grid and env, clients built with CONSUME_CRC.
_CRC_BASES = ["streaming", "whole_transfer", "framed", "tls", "tls_framed", "udp_k1400",
              "streaming_naive", "whole_transfer_naive"]
for _base in _CRC_BASES:
    SCENARIOS[f"{_base}_crc"] = dict(SCENARIOS[_base], bench=SCENARIOS[_base]["bench"] + "_crc",
                                     env=dict(SCENARIOS[_base].get("env", {})))
_CRC_NAMES = [f"{b}_crc" for b in _CRC_BASES]

DRY_RUN = os.environ.get("DRY_RUN", "").strip().lower() not in ("", "0", "false", "no")


def _requested_names():
    raw = os.environ.get("RUN_SCENARIOS", "").strip()
    names = [n for n in raw.replace(",", " ").split() if n]
    return [x for n in names for x in (_CRC_NAMES if n == "crc" else [n])]


def active_scenarios():
    names = _requested_names()
    if not names:
        return [n for n in SCENARIOS if n not in _CRC_NAMES]
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        raise SystemExit(f"Unknown scenario(s) in RUN_SCENARIOS: {unknown}. "
                          f"Valid: {list(SCENARIOS.keys())}")
    return names


def _payload_bytes():
    try:
        return os.path.getsize(PAYLOAD_FILE)
    except OSError:
        return None


def _expected_crc(*manifest):
    tool = "../tls/crc32c_expected"
    if not os.path.exists(tool):
        raise SystemExit(f"{tool} not found: build.sh builds it (needed by the *_crc scenarios)")
    out = subprocess.run([tool, PAYLOAD_FILE, *manifest], check=True, capture_output=True, text=True)
    return out.stdout.strip()


_size = _payload_bytes()
if _size is not None:
    for _spec in SCENARIOS.values():
        _spec.setdefault("env", {})["PAYLOAD_BYTES"] = str(_size)
if not DRY_RUN and _size is not None and any(n in _CRC_NAMES for n in _requested_names()):
    # The framed servers send one body per manifest size, cut from the payload in order.
    _crc_whole, _crc_framed = _expected_crc(), _expected_crc(_MANIFEST)
    for _name in _CRC_NAMES:
        if not _name.startswith("udp"):
            SCENARIOS[_name]["env"]["EXPECTED_CRC32C"] = _crc_framed if "framed" in _name else _crc_whole


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
