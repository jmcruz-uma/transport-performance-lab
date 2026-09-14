#!/usr/bin/env python3
"""D2 comparability check: after a `tls` / `tls_framed` campaign has run for
each arm, confirm every arm's server and client actually negotiated the
identical TLS parameters (linked OpenSSL version, TLS version, cipher suite,
ALPN).

Run this ONCE per campaign on the measurement machine (e.g. right after
building everything, or right after the first `tls`/`tls_framed` run of each
arm) -- not per benchmark iteration. The guarantee that makes a per-iteration
check pointless already holds within one arm: its server and client are built
in the same step from the same tls/tls_common.hpp, so they cannot disagree
with each other without the handshake failing outright (which already shows
up as a connection error, not a divergent TLS_IDENTITY). What this script
catches instead is DRIFT ACROSS ARMS or across time -- e.g. one arm rebuilt
after a system OpenSSL upgrade while the others were not, or a stray
TLS_CERT/TLS_CA env var pointing somewhere unexpected.

Reads the TLS_IDENTITY lines run_bench.py already captures into each arm's
results/<label>/logs/*.log (server stdout+stderr, every bench process's
stderr) -- no extra instrumentation needed. <label> is the plain scenario
name (loopback baseline) OR any of its D7 netem-swept variants
(f"{scenario}__netem_rtt_<R>ms_loss_<L>pct", from netem/run_rtt_sweep.sh) --
every one of them is scanned and pooled together, since simulated RTT/loss
has no business changing which TLS parameters got negotiated.

As of 2026-09-15 this is invoked automatically (non-fatally -- a mismatch is
logged loudly, not treated as a reason to abort a campaign whose data is
already collected) by collect_global_results.sh, so it no longer has to be
remembered and run by hand while someone is standing at the machine.

Usage:
    tls/check_identity.py [--scenario tls|tls_framed|both] [--arm ARM ...] [--root PATH]

Exit 0 with a summary if every arm agrees; exit 1 with the mismatching
fields otherwise. Also flags a log file whose own repeated readings disagree
with each other (a flakier problem than cross-arm drift, worth knowing about
either way).
"""

import argparse
import re
import sys
from pathlib import Path

ARMS = ["asio", "bsd-sockets", "capy-corosio", "taps-asio"]
SCENARIOS = ["tls", "tls_framed"]

# Common to every arm's TLS_IDENTITY print (tls/tls_common.hpp::print_tls_identity,
# and the taps-asio / capy-corosio arms' own equivalents -- same text shape).
IDENTITY_RE = re.compile(
    r'TLS_IDENTITY who=(?P<who>\S+) openssl="(?P<openssl>[^"]*)" '
    r'version=(?P<version>\S+) cipher=(?P<cipher>\S+) alpn=(?P<alpn>\S*)'
)

FIELDS = ("openssl", "version", "cipher", "alpn")


def label_dirs_for_scenario(root: Path, arm: str, scenario: str):
    """Every results/<label>/ that belongs to this scenario -- the plain
    label itself (loopback baseline) plus every D7 netem-swept variant
    (netem/run_rtt_sweep.sh names those f"{scenario}__netem_rtt_<R>ms_loss_<L>pct").
    TLS identity (OpenSSL version/cipher/ALPN) shouldn't depend on simulated
    RTT/loss at all, so pooling every label's readings together and checking
    they all still agree is exactly the right check -- and it's the only way
    this script sees the netem-swept results at all, since they never live
    under the plain results/<scenario>/ path once relocated."""
    results_root = root / arm / "results"
    if not results_root.is_dir():
        return []
    dirs = []
    for p in sorted(results_root.iterdir()):
        if not p.is_dir():
            continue
        if p.name == scenario or p.name.startswith(f"{scenario}__"):
            dirs.append(p)
    return dirs


def find_identities(root: Path, arm: str, scenario: str):
    """{who: fields} for the first reading of each `who` found across every
    results/<label>/logs/ this scenario has (baseline + every netem grid
    point), plus a synthetic f"{who}!inconsistent" entry (with the LAST
    disagreeing reading) if later log files disagree with the first. `who`
    is tagged with the originating label so a mismatch report says exactly
    which grid point it came from."""
    found = {}
    for label_dir in label_dirs_for_scenario(root, arm, scenario):
        logs_dir = label_dir / "logs"
        if not logs_dir.is_dir():
            continue
        label = label_dir.name
        for log_path in sorted(logs_dir.glob("*.log")):
            try:
                text = log_path.read_text(errors="replace")
            except OSError:
                continue
            for m in IDENTITY_RE.finditer(text):
                who = f"{m.group('who')}[{label}]"
                fields = {k: m.group(k) for k in FIELDS}
                if who not in found:
                    found[who] = fields
                elif found[who] != fields:
                    found[f"{who}!inconsistent"] = fields
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=SCENARIOS + ["both"], default="both")
    ap.add_argument("--arm", action="append", choices=ARMS,
                    help="repeatable; default is all four arms")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="repo root (default: parent of tls/)")
    args = ap.parse_args()

    scenarios = SCENARIOS if args.scenario == "both" else [args.scenario]
    arms = args.arm or ARMS

    overall_ok = True
    for scenario in scenarios:
        print(f"=== {scenario} ===")
        pooled = []  # (arm, who, fields) for every clean reading
        any_data = False

        for arm in arms:
            identities = find_identities(args.root, arm, scenario)
            if not identities:
                print(f"  {arm:14s} NO DATA "
                      f"(results/{scenario}/logs not found or empty -- "
                      f"run this arm's {scenario} campaign at least once first)")
                overall_ok = False
                continue
            any_data = True
            for who, fields in identities.items():
                inconsistent = who.endswith("!inconsistent")
                tag = "  <-- this arm's own logs disagree with themselves" if inconsistent else ""
                print(f"  {arm:14s} {who:20s} openssl={fields['openssl']!r} "
                      f"version={fields['version']} cipher={fields['cipher']} "
                      f"alpn={fields['alpn']!r}{tag}")
                if inconsistent:
                    overall_ok = False
                else:
                    pooled.append((arm, who, fields))

        if pooled:
            reference = pooled[0][2]
            mismatches = [(arm, who, f) for arm, who, f in pooled if f != reference]
            if mismatches:
                overall_ok = False
                print(f"  MISMATCH: not every arm negotiated the same TLS "
                      f"parameters for {scenario} (reference = {pooled[0][0]}/"
                      f"{pooled[0][1]}: {reference}):")
                for arm, who, f in mismatches:
                    print(f"    {arm}/{who}: {f}")
            else:
                print(f"  OK: {len(pooled)} readings across {len(arms)} arms agree")
        elif any_data:
            overall_ok = False
        print()

    if not overall_ok:
        print("check_identity: FAILED -- do not treat this campaign's results as "
              "comparable across arms until this is resolved", file=sys.stderr)
        return 1
    print("check_identity: all arms agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
