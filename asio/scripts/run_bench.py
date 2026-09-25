#!/usr/bin/env python3

import csv
import json
import math
import os
import signal
import socket
import statistics
import subprocess
import sys
import time
import textwrap
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from bench_scenarios import SCENARIOS, DRY_RUN, active_scenarios, is_done, mark_done


# =========================
# PATHS
# =========================
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
ROOT_DIR = PROJECT_DIR.parent

IDLE_BASELINE_JSON = ROOT_DIR / "idle_baseline.json"
USE_IDLE_BASELINE = True
DEFAULT_IDLE_POWER_W = 0.0

# =========================
# OUTPUT / NOISE CONTROL
# =========================
QUIET = False
PRINT_FINAL_SUMMARY = False
GENERATE_PLOTS = True
GENERATE_PDF = True
GENERATE_COMPARISON_PDF = True

# =========================
# BENCHMARK CONFIG
# =========================
BUILD_DIRS = {
    "gcc": "./build-gcc",
    "clang": "./build-clang",
}

PORTS = {
    "gcc": 8120,
    "clang": 8121,
}

COMPILERS = ["gcc"]  # clang dropped to fit D7 in ~4-5 days; baseline showed <3% gcc/clang delta

FILE_TO_SERVE = "../files/100MB.bin"
# Overridden by the D7 netem/netns sweep (netem/run_rtt_sweep.sh) so the server
# is reached across the netns+veth link instead of loopback; see netem/netem_common.sh.
HOST = os.environ.get("NETEM_SERVER_HOST", "127.0.0.1")
# When set, the server process is launched inside this network namespace
# (`ip netns exec <NETEM_SERVER_NETNS> ...`) instead of the root namespace.
NETEM_SERVER_NETNS = os.environ.get("NETEM_SERVER_NETNS", "")
# Port-freeness probing (can_bind_port/find_free_port/kill_processes_on_port)
# always happens against the root namespace's own address, never HOST: a
# freshly-created netns has its own independent, empty port space, so
# bind-probing it is meaningless -- and literally impossible from the root
# namespace, since you cannot bind() a foreign namespace's address. Only the
# real connect target (is_port_open/wait_for_server, and the bench binary's
# own --server_ip) should ever use HOST.
PROBE_HOST = "127.0.0.1"

ENERGY_PATH = "/sys/class/powercap/intel-rapl:0/energy_uj"
MAX_ENERGY_PATH = "/sys/class/powercap/intel-rapl:0/max_energy_range_uj"

RESULTS_DIR = Path("./results")
RAW_DIR = RESULTS_DIR / "raw"
PLOTS_DIR = RESULTS_DIR / "plots"
REPORTS_DIR = RESULTS_DIR / "reports"
LOGS_DIR = RESULTS_DIR / "logs"

MACRO_BENCH_CASES = [1, 2, 4, 8, 16]
SERVER_THREADS = [1, 2, 4, 8]
MACRO_REPETITIONS = int(os.environ.get("MACRO_REPETITIONS", "25"))

BENCH_ARGS = [
    "--benchmark_out_format=json"
]

FINAL_RESULTS = RAW_DIR / "macro_bench_results.json"
SUMMARY_RESULTS = RAW_DIR / "macro_bench_summary.json"
CSV_RESULTS = RAW_DIR / "macro_bench_results.csv"

MAIN_PDF_WITH_RAW = REPORTS_DIR / "macro_bench_report_with_raw.pdf"
MAIN_PDF_NO_RAW = REPORTS_DIR / "macro_bench_report_no_raw.pdf"
COMPARISON_PDF_WITH_RAW = REPORTS_DIR / "macro_bench_comparison_report_with_raw.pdf"
COMPARISON_PDF_NO_RAW = REPORTS_DIR / "macro_bench_comparison_report_no_raw.pdf"

# Backward-compatible aliases
PDF_RESULTS = MAIN_PDF_WITH_RAW
COMPARISON_PDF_RESULTS = COMPARISON_PDF_WITH_RAW

# =========================
# SCENARIOS (E0 streaming / E1 whole_object / E3 framed / E4 udp_k64,k1400)
# =========================
CURRENT_SCENARIO = None
SERVER_DIR = "tcpserver"
BENCH_NAME = "bench_tcp"
SCENARIO_ENV = {}
IS_UDP_SCENARIO = False


def _child_env():
    return {**os.environ, **{k: str(v) for k, v in SCENARIO_ENV.items()}}


def _maybe_netns_wrap(cmd):
    """Prefix `cmd` with `ip netns exec <NETEM_SERVER_NETNS>` when the D7 netem
    sweep has set that env var (see netem/netem_common.sh); a no-op otherwise.
    Uses `sudo -n` unless already root -- build.sh installs a NOPASSWD rule
    scoped to tc/ip precisely so this never has to prompt."""
    if not NETEM_SERVER_NETNS:
        return cmd
    prefix = ["ip", "netns", "exec", NETEM_SERVER_NETNS]
    if os.geteuid() != 0:
        prefix = ["sudo", "-n"] + prefix
    return prefix + cmd


def _activate_scenario(name):
    """Rebind the scenario-dependent module globals before a scenario's campaign."""
    global CURRENT_SCENARIO, SERVER_DIR, BENCH_NAME, SCENARIO_ENV, IS_UDP_SCENARIO
    global MACRO_BENCH_CASES, SERVER_THREADS
    global RESULTS_DIR, RAW_DIR, PLOTS_DIR, REPORTS_DIR, LOGS_DIR
    global FINAL_RESULTS, SUMMARY_RESULTS, CSV_RESULTS
    global MAIN_PDF_WITH_RAW, MAIN_PDF_NO_RAW
    global COMPARISON_PDF_WITH_RAW, COMPARISON_PDF_NO_RAW
    global PDF_RESULTS, COMPARISON_PDF_RESULTS

    spec = SCENARIOS[name]
    CURRENT_SCENARIO = name
    SERVER_DIR = spec["server"]
    IS_UDP_SCENARIO = (SERVER_DIR == "udpserver")
    BENCH_NAME = spec["bench"]
    SCENARIO_ENV = dict(spec.get("env", {}))
    MACRO_BENCH_CASES = list(spec["cases"])
    SERVER_THREADS = list(spec["threads"])

    RESULTS_DIR = Path("./results") / name
    RAW_DIR = RESULTS_DIR / "raw"
    PLOTS_DIR = RESULTS_DIR / "plots"
    REPORTS_DIR = RESULTS_DIR / "reports"
    LOGS_DIR = RESULTS_DIR / "logs"

    FINAL_RESULTS = RAW_DIR / "macro_bench_results.json"
    SUMMARY_RESULTS = RAW_DIR / "macro_bench_summary.json"
    CSV_RESULTS = RAW_DIR / "macro_bench_results.csv"

    MAIN_PDF_WITH_RAW = REPORTS_DIR / "macro_bench_report_with_raw.pdf"
    MAIN_PDF_NO_RAW = REPORTS_DIR / "macro_bench_report_no_raw.pdf"
    COMPARISON_PDF_WITH_RAW = REPORTS_DIR / "macro_bench_comparison_report_with_raw.pdf"
    COMPARISON_PDF_NO_RAW = REPORTS_DIR / "macro_bench_comparison_report_no_raw.pdf"
    PDF_RESULTS = MAIN_PDF_WITH_RAW
    COMPARISON_PDF_RESULTS = COMPARISON_PDF_WITH_RAW

# =========================
# CASE-LEVEL SETTLE / CACHE CONTROL
# =========================
CASE_CACHE_TRASH_ENABLED = True
CASE_CACHE_TRASH_SIZE_MB = 1024
CASE_DROP_CACHES = True
CASE_COOLDOWN_SECONDS = 20
# Short pause between repetitions of the SAME case: lets ephemeral ports and
# short-lived processes clear and the CPU shed the previous burst's heat,
# without dropping the warm page cache mid-case (that's a legitimate part of
# what's being measured, not noise). Deliberately much shorter than the
# case-boundary cooldown above, which resets state for a genuinely new
# configuration.
REP_COOLDOWN_SECONDS = 3

# =========================
# SERVER PROCESS
# =========================
SERVER_WAIT_TIMEOUT_SECONDS = 60.0
SERVER_STOP_TIMEOUT_SECONDS = 5.0
PORT_RETRY_SPAN = 200
# Found 2026-09-15 auditing the harness ahead of the real-machine deployment:
# the bench client wait below used to be an unbounded proc.wait() -- a single
# hung client would silently stall the ENTIRE campaign forever, with nothing
# left to notice for days. Bounded here instead: a repetition that doesn't
# finish in time is killed and counted as failed (parse_benchmark_json
# already treats a missing/invalid output file as `failed`), and the
# campaign moves on. This constant went 600 -> 4200 and 4200 still wasn't
# enough: real D7 runs (2026-09-22, 2026-09-25) kept killing a real fraction
# (~24% at RTT=0/loss=5%) of legitimately-completing repetitions, and killed
# reps are NOT retried (see the plain `for rep in range(MACRO_REPETITIONS)`
# loop below) -- every kill both wastes the time already spent AND silently
# shrinks and biases the sample (the slowest, most informative tail is
# exactly what gets thrown away). No genuine hang has ever actually been
# observed across ~9 days of real runs; every "slow" case measured so far
# has been slow-but-finite. Raised to 28800s (8h, ~12x the worst
# legitimately-completing case measured, 2326s) so this stops being the
# thing that decides the sample -- it exists only to catch something
# actually broken, not to bound how long a real transfer is allowed to take.
BENCH_CLIENT_TIMEOUT_SECONDS = float(os.environ.get("BENCH_CLIENT_TIMEOUT_SECONDS", "28800"))

# =========================
# PDF / TABLE TUNING
# =========================
TABLE_WRAP_MAIN = 20
TABLE_WRAP_COMPARISON = 18
TABLE_WRAP_RAW = 14
TABLE_FONT_SIZE = 8
TABLE_HEADER_FONT_SIZE = 9
TABLE_VERTICAL_SCALE = 3.0
TABLE_COMPARISON_VERTICAL_SCALE = 3.3
TABLE_RAW_VERTICAL_SCALE = 1.9


# =========================
# BASIC UTILITIES
# =========================
def log(msg):
    if not QUIET:
        print(msg, flush=True)



def ensure_results_dir():
    for directory in [RESULTS_DIR, RAW_DIR, PLOTS_DIR, REPORTS_DIR, LOGS_DIR]:
        directory.mkdir(parents=True, exist_ok=True)
        os.chmod(directory, 0o777)


# =========================
# IDLE BASELINE
# =========================
def load_idle_power_w():
    if not USE_IDLE_BASELINE:
        return DEFAULT_IDLE_POWER_W

    if not IDLE_BASELINE_JSON.exists():
        log(
            f"WARNING: {IDLE_BASELINE_JSON} does not exist, "
            f"using DEFAULT_IDLE_POWER_W={DEFAULT_IDLE_POWER_W}"
        )
        return DEFAULT_IDLE_POWER_W

    try:
        with open(IDLE_BASELINE_JSON, "r", encoding="utf-8") as f:
            data = json.load(f)

        value = float(data.get("watts_mean", DEFAULT_IDLE_POWER_W))
        log(f"Using idle baseline from {IDLE_BASELINE_JSON}: {value:.6f} W")
        return value

    except Exception as e:
        log(f"WARNING: error reading {IDLE_BASELINE_JSON}: {e}")
        log(f"Using DEFAULT_IDLE_POWER_W={DEFAULT_IDLE_POWER_W}")
        return DEFAULT_IDLE_POWER_W


IDLE_POWER_W = load_idle_power_w()


# =========================
# ENERGY
# =========================
def _check_rapl_available():
    try:
        with open(ENERGY_PATH) as f:
            f.read()
        return True
    except OSError:
        return False


RAPL_AVAILABLE = _check_rapl_available()
if not RAPL_AVAILABLE:
    log(f"WARNING: RAPL energy counter not readable at {ENERGY_PATH} -- all "
        f"energy_j values will be 0 for this run (expected on machines without "
        f"RAPL, e.g. WSL2/VMs; on the real measurement machine this should never "
        f"happen, since preflight.sh already verifies RAPL is readable before "
        f"the campaign starts).")


def read_energy():
    if not RAPL_AVAILABLE:
        return 0
    with open(ENERGY_PATH) as f:
        return int(f.read().strip())



def read_max_energy():
    with open(MAX_ENERGY_PATH) as f:
        return int(f.read().strip())



def energy_delta_j(e1, e2):
    if e2 >= e1:
        delta_uj = e2 - e1
    else:
        max_range = read_max_energy()
        delta_uj = (max_range - e1) + e2

    return delta_uj / 1_000_000.0


# =========================
# PATH HELPERS
# =========================
def get_file_size():
    if DRY_RUN and not os.path.exists(FILE_TO_SERVE):
        return 100 * 1024 * 1024
    return os.path.getsize(FILE_TO_SERVE)



def get_server_bin(compiler):
    return os.path.join(BUILD_DIRS[compiler], SERVER_DIR, SERVER_DIR)



def get_bench_bin(compiler):
    return os.path.join(BUILD_DIRS[compiler], "benchmarks", BENCH_NAME)



def get_server_log_paths(compiler, server_threads):
    stdout_path = LOGS_DIR / f"server_{compiler}_threads_{server_threads}_stdout.log"
    stderr_path = LOGS_DIR / f"server_{compiler}_threads_{server_threads}_stderr.log"
    return stdout_path, stderr_path



def get_bench_stderr_log_path(compiler, server_threads, case_clients, repetition, index):
    return LOGS_DIR / (
        f"bench_{compiler}_threads_{server_threads}_{case_clients}_{repetition}_{index}.stderr.log"
    )



def get_micro_json_path(compiler, server_threads, case_clients, repetition, index):
    return RAW_DIR / (
        f"micro_{compiler}_threads_{server_threads}_{case_clients}_{repetition}_{index}.json"
    )



def get_port(compiler):
    return PORTS[compiler]



def set_port(compiler, port):
    PORTS[compiler] = port



def read_text_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except Exception:
        return ""


# =========================
# PORT MANAGEMENT
# =========================
def is_port_open(host, port, timeout=0.5):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def is_udp_server_ready(pid, port):
    """UDP has no handshake, so neither a plain TCP connect (always fails
    against a UDP socket) nor a UDP 'connect' (always succeeds locally,
    even with nobody listening) can tell us the server is up. Read the
    bound local ports straight out of /proc/<pid>/net/udp{,6} instead --
    namespace-correct by construction (reads exactly that process's own
    namespace view, whether it's still in the root ns or `ip netns exec`
    moved it into the D7 topology's netns) and needs no protocol-level
    probe. Found 2026-09-18: every udp_k64/udp_k1400 run in the first real
    campaign timed out and was counted as a failure because wait_for_server
    used is_port_open() unconditionally -- the udpserver process itself was
    fine the whole time, only the readiness check was wrong for UDP."""
    for proto_file in ("udp", "udp6"):
        try:
            with open(f"/proc/{pid}/net/{proto_file}") as f:
                next(f, None)  # header line
                for line in f:
                    fields = line.split()
                    if len(fields) < 2:
                        continue
                    local_hex_port = fields[1].rsplit(":", 1)[-1]
                    try:
                        if int(local_hex_port, 16) == port:
                            return True
                    except ValueError:
                        continue
        except OSError:
            continue
    return False



def can_bind_port(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()



def find_free_port(start_port, host=PROBE_HOST, span=PORT_RETRY_SPAN):
    for port in range(start_port, start_port + span):
        if can_bind_port(host, port):
            return port
    raise RuntimeError(
        f"Could not find a free port in range [{start_port}, {start_port + span - 1}]"
    )



def _run_optional_command(cmd):
    try:
        subprocess.run(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except Exception:
        pass



def kill_processes_on_port(port):
    _run_optional_command(["fuser", "-k", "-TERM", f"{port}/tcp"])
    time.sleep(0.5)

    if can_bind_port(PROBE_HOST, port):
        return

    _run_optional_command(["fuser", "-k", "-KILL", f"{port}/tcp"])
    time.sleep(0.5)

    if can_bind_port(PROBE_HOST, port):
        return

    try:
        res = subprocess.run(
            ["lsof", "-ti", f"tcp:{port}"],
            capture_output=True,
            text=True,
            check=False,
        )
        pids = [x.strip() for x in res.stdout.splitlines() if x.strip()]
        for pid in pids:
            _run_optional_command(["kill", "-TERM", pid])
        if pids:
            time.sleep(0.5)
        if not can_bind_port(PROBE_HOST, port):
            for pid in pids:
                _run_optional_command(["kill", "-KILL", pid])
            if pids:
                time.sleep(0.5)
    except Exception:
        pass



def prepare_server_port(compiler):
    preferred = get_port(compiler)
    kill_processes_on_port(preferred)

    if can_bind_port(PROBE_HOST, preferred):
        return preferred

    new_port = find_free_port(preferred + 1)
    log(f"Port {preferred} is still busy, switching {compiler} to port {new_port}")
    set_port(compiler, new_port)
    return new_port


# =========================
# SERVER LIFECYCLE
# =========================
def start_server(compiler, server_threads):
    server_bin = get_server_bin(compiler)
    port = prepare_server_port(compiler)

    if not os.path.exists(server_bin):
        raise FileNotFoundError(f"Server binary not found for {compiler}: {server_bin}")

    stdout_path, stderr_path = get_server_log_paths(compiler, server_threads)

    stdout_file = open(stdout_path, "w")
    stderr_file = open(stderr_path, "w")

    proc = subprocess.Popen(
        _maybe_netns_wrap([server_bin, FILE_TO_SERVE, str(port), str(server_threads)]),
        stdout=stdout_file,
        stderr=stderr_file,
        env=_child_env(),
        text=True,
        preexec_fn=os.setsid,
    )

    return proc, stdout_file, stderr_file, port



def stop_server(proc, stdout_file=None, stderr_file=None, port=None):
    try:
        if proc is not None and proc.poll() is None:
            try:
                pgid = os.getpgid(proc.pid)
                os.killpg(pgid, signal.SIGTERM)
            except Exception:
                try:
                    proc.terminate()
                except Exception:
                    pass

            try:
                proc.wait(timeout=SERVER_STOP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                try:
                    pgid = os.getpgid(proc.pid)
                    os.killpg(pgid, signal.SIGKILL)
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass

                try:
                    proc.wait(timeout=SERVER_STOP_TIMEOUT_SECONDS)
                except Exception:
                    pass
    finally:
        if stdout_file is not None and not stdout_file.closed:
            stdout_file.close()

        if stderr_file is not None and not stderr_file.closed:
            stderr_file.close()

        if port is not None:
            kill_processes_on_port(port)

        time.sleep(1.0)



def wait_for_server(proc, host, port, compiler, server_threads, timeout=SERVER_WAIT_TIMEOUT_SECONDS):
    start = time.perf_counter()

    while (time.perf_counter() - start) < timeout:
        if proc.poll() is not None:
            stdout_path, stderr_path = get_server_log_paths(compiler, server_threads)
            stdout_text = read_text_file(stdout_path)
            stderr_text = read_text_file(stderr_path)

            raise RuntimeError(
                f"The server for {compiler} with {server_threads} threads exited before becoming ready.\n"
                f"Port: {port}\n"
                f"stdout log: {stdout_path}\n"
                f"stderr log: {stderr_path}\n"
                f"--- STDOUT ---\n{stdout_text}\n"
                f"--- STDERR ---\n{stderr_text}"
            )

        ready = is_udp_server_ready(proc.pid, port) if IS_UDP_SCENARIO else is_port_open(host, port)
        if ready:
            return True

        time.sleep(0.2)

    stdout_path, stderr_path = get_server_log_paths(compiler, server_threads)
    stdout_text = read_text_file(stdout_path)
    stderr_text = read_text_file(stderr_path)

    raise RuntimeError(
        f"The server for {compiler} with {server_threads} threads did not become ready in time.\n"
        f"Port: {port}\n"
        f"stdout log: {stdout_path}\n"
        f"stderr log: {stderr_path}\n"
        f"--- STDOUT ---\n{stdout_text}\n"
        f"--- STDERR ---\n{stderr_text}"
    )


# =========================
# CASE-LEVEL COOLING / CACHE
# =========================
def case_level_cache_trash():
    if not CASE_CACHE_TRASH_ENABLED:
        return

    log(f"Case-level cache trashing best-effort ({CASE_CACHE_TRASH_SIZE_MB} MB)...")

    subprocess.run(
        [
            sys.executable,
            "-c",
            (
                "size_mb = int(__import__('os').environ['CASE_CACHE_MB']);"
                "chunk = 1024 * 1024;"
                "buf = bytearray(chunk);"
                "acc = 0;"
                "for i in range(size_mb):\n"
                "    for j in range(0, len(buf), 4096):\n"
                "        buf[j] = (i + j) & 0xFF;"
                "        acc ^= buf[j];"
                "print(acc)"
            )
        ],
        env={**os.environ, "CASE_CACHE_MB": str(CASE_CACHE_TRASH_SIZE_MB)},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

    if CASE_DROP_CACHES and os.access("/proc/sys/vm/drop_caches", os.W_OK):
        log("Dropping page cache (case-level best-effort)")
        subprocess.run(["sync"], check=False)
        try:
            with open("/proc/sys/vm/drop_caches", "w") as f:
                f.write("3\n")
        except Exception:
            pass



def settle_between_cases():
    if CASE_COOLDOWN_SECONDS <= 0:
        return
    log(f"Cooling down between cases for {CASE_COOLDOWN_SECONDS} seconds...")
    time.sleep(CASE_COOLDOWN_SECONDS)



def settle_between_repetitions():
    if REP_COOLDOWN_SECONDS <= 0:
        return
    time.sleep(REP_COOLDOWN_SECONDS)



def settle_before_campaign():
    # Same reset as a case-boundary, but for a bigger transition: a new
    # (compiler, server_threads) campaign is about to start (which also
    # covers scenario-to-scenario transitions, since each scenario's first
    # campaign goes through here too).
    case_level_cache_trash()
    if CASE_COOLDOWN_SECONDS <= 0:
        return
    log(f"Cooling down for {CASE_COOLDOWN_SECONDS} seconds before starting a new compiler/thread-count campaign...")
    time.sleep(CASE_COOLDOWN_SECONDS)


# =========================
# BENCH EXECUTION
# =========================
def start_bench_instance(compiler, server_threads, case_clients, repetition, index, port):
    bench_bin = get_bench_bin(compiler)

    if not os.path.exists(bench_bin):
        raise FileNotFoundError(f"Benchmark binary not found for {compiler}: {bench_bin}")

    output_json = get_micro_json_path(compiler, server_threads, case_clients, repetition, index)
    stderr_log = get_bench_stderr_log_path(compiler, server_threads, case_clients, repetition, index)

    if output_json.exists():
        output_json.unlink()

    cmd = (
        [bench_bin]
        + BENCH_ARGS
        + [f"--server_port={port}"]
        + [f"--server_ip={HOST}"]
        + [f"--benchmark_out={output_json}"]
    )

    err_file = open(stderr_log, "w")

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=err_file,
        env=_child_env(),
        preexec_fn=os.setsid,
    )

    return proc, output_json, err_file



def stop_bench_process(proc):
    try:
        if proc is not None and proc.poll() is None:
            try:
                pgid = os.getpgid(proc.pid)
                os.killpg(pgid, signal.SIGTERM)
            except Exception:
                try:
                    proc.terminate()
                except Exception:
                    pass

            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    pgid = os.getpgid(proc.pid)
                    os.killpg(pgid, signal.SIGKILL)
                except Exception:
                    try:
                        proc.kill()
                    except Exception:
                        pass
                try:
                    proc.wait(timeout=3)
                except Exception:
                    pass
    except Exception:
        pass



def parse_benchmark_json(path):
    try:
        with open(path) as f:
            data = json.load(f)

        total_iterations = 0
        downloaded_bytes = 0

        for entry in data.get("benchmarks", []):
            if entry.get("error_occurred", False):
                return False, 0, 0

            run_type = entry.get("run_type")

            if run_type is None or run_type == "iteration":
                total_iterations += int(entry.get("iterations", 0))

                if "downloaded_bytes" in entry:
                    downloaded_bytes += int(float(entry.get("downloaded_bytes", 0)))

        if total_iterations <= 0 and downloaded_bytes <= 0:
            return False, 0, 0

        return True, total_iterations, downloaded_bytes

    except Exception:
        return False, 0, 0



def run_macro_bench_case(compiler, server_threads, num_benches, repetition, file_size_bytes, port):
    processes = []
    outputs = []

    e1 = read_energy()
    t1 = time.perf_counter()

    try:
        for i in range(num_benches):
            proc, out_json, err_file = start_bench_instance(
                compiler,
                server_threads,
                num_benches,
                repetition,
                i,
                port,
            )
            processes.append((proc, out_json, err_file))
            outputs.append(out_json)

        for proc, _, err_file in processes:
            try:
                proc.wait(timeout=BENCH_CLIENT_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                log(
                    f"WARNING: bench client (pid={proc.pid}) did not finish within "
                    f"{BENCH_CLIENT_TIMEOUT_SECONDS}s -- killing it and counting this "
                    f"repetition as failed instead of hanging the whole campaign."
                )
                stop_bench_process(proc)
            err_file.close()
    except KeyboardInterrupt:
        for proc, _, err_file in processes:
            stop_bench_process(proc)
            try:
                err_file.close()
            except Exception:
                pass
        raise

    t2 = time.perf_counter()
    e2 = read_energy()

    elapsed_s = t2 - t1
    elapsed_ms = elapsed_s * 1000.0
    energy_j_raw = energy_delta_j(e1, e2)
    idle_energy_j_estimated = IDLE_POWER_W * elapsed_s
    energy_j_net = max(0.0, energy_j_raw - idle_energy_j_estimated)

    total_iterations = 0
    total_downloaded_bytes = 0
    valid_json_files = []
    success = 0
    failed = 0

    for path in outputs:
        if os.path.exists(path):
            valid, iters, bytes_downloaded = parse_benchmark_json(path)
            if valid:
                total_iterations += iters
                total_downloaded_bytes += bytes_downloaded
                valid_json_files.append(str(path))
                success += 1
            else:
                failed += 1
        else:
            failed += 1

    # Prefer the benchmark's own downloaded_bytes counter (accurate under UDP
    # loss, where an "iteration" can legitimately complete with a short
    # transfer -- see bench_udp.cpp) over iterations*file_size, which silently
    # assumed every iteration delivered the whole file. Falls back to the old
    # estimate only if the counter is unavailable/zero (e.g. older binaries).
    real_bytes = total_downloaded_bytes
    if real_bytes <= 0:
        real_bytes = total_iterations * file_size_bytes

    throughput_mib_s = (
        real_bytes / (1024 * 1024) / elapsed_s
        if elapsed_s > 0 else 0.0
    )

    return {
        "compiler": compiler,
        "port": port,
        "server_threads": server_threads,
        "parallel_bench_processes": num_benches,
        "repetition": repetition,
        "success": success,
        "failed": failed,
        "total_iterations": total_iterations,
        "downloads_per_process": (total_iterations / success) if success > 0 else 0.0,
        "elapsed_s": elapsed_s,
        "elapsed_ms": elapsed_ms,
        "idle_power_w": IDLE_POWER_W,
        "energy_j_raw": energy_j_raw,
        "idle_energy_j_estimated": idle_energy_j_estimated,
        "energy_j": energy_j_net,
        "real_transferred_bytes": real_bytes,
        "throughput_mib_s": throughput_mib_s,
        "benchmark_json_files": valid_json_files
    }



def run_campaign_for_compiler_and_threads(compiler, server_threads):
    server_bin = get_server_bin(compiler)
    bench_bin = get_bench_bin(compiler)
    if not os.path.exists(server_bin) or not os.path.exists(bench_bin):
        missing = server_bin if not os.path.exists(server_bin) else bench_bin
        log(f"[{compiler}] SKIP scenario '{CURRENT_SCENARIO}': {missing} not found -- "
            f"either this project/compiler combination does not implement this scenario "
            f"(e.g. async-berkeley has no TLS binaries) or the build is incomplete. "
            f"Skipping this (compiler, scenario) combination instead of raising, which "
            f"used to abort the whole run.sh (set -e) and silently drop every project "
            f"queued after this one.")
        return []

    settle_before_campaign()

    file_size_bytes = get_file_size()
    results = []

    log(f"Starting server for {compiler} on port {get_port(compiler)} with {server_threads} threads...")
    server, server_stdout, server_stderr, actual_port = start_server(compiler, server_threads)

    wait_for_server(server, HOST, actual_port, compiler, server_threads)

    try:
        log(f"Running benchmark campaign for {compiler} with {server_threads} server threads...")

        for benches in MACRO_BENCH_CASES:
            case_level_cache_trash()
            settle_between_cases()

            for rep in range(MACRO_REPETITIONS):
                log(
                    f"[{compiler}][server_threads={server_threads}] "
                    f"Running {benches} parallel benchmark process(es) on port {actual_port}..."
                )
                result = run_macro_bench_case(
                    compiler,
                    server_threads,
                    benches,
                    rep,
                    file_size_bytes,
                    actual_port,
                )
                results.append(result)
                settle_between_repetitions()
    finally:
        stop_server(server, server_stdout, server_stderr, actual_port)

    return results


# =========================
# STATISTICS
# =========================
def percentile(sorted_values, p):
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]

    k = (len(sorted_values) - 1) * (p / 100.0)
    f = math.floor(k)
    c = math.ceil(k)

    if f == c:
        return sorted_values[int(k)]

    d0 = sorted_values[f] * (c - k)
    d1 = sorted_values[c] * (k - f)
    return d0 + d1



def compute_stats(values):
    if not values:
        return None

    sorted_values = sorted(values)

    return {
        "count": len(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
        "p25": percentile(sorted_values, 25),
        "p50": percentile(sorted_values, 50),
        "p95": percentile(sorted_values, 95),
    }



def summarize_results(results):
    grouped = {}

    for item in results:
        key = (
            item["compiler"],
            item["server_threads"],
            item["parallel_bench_processes"],
        )
        grouped.setdefault(key, []).append(item)

    summary = []

    for (compiler, server_threads, parallel_bench_processes), items in sorted(grouped.items()):
        elapsed_values = [x["elapsed_s"] for x in items]
        elapsed_ms_values = [x.get("elapsed_ms", x["elapsed_s"] * 1000.0) for x in items]
        energy_raw_values = [x["energy_j_raw"] for x in items]
        idle_estimated_values = [x["idle_energy_j_estimated"] for x in items]
        energy_values = [x["energy_j"] for x in items]
        throughput_values = [x["throughput_mib_s"] for x in items]
        downloads_values = [x["downloads_per_process"] for x in items]
        iterations_values = [x["total_iterations"] for x in items]
        success_values = [x["success"] for x in items]
        failed_values = [x["failed"] for x in items]

        summary.append({
            "compiler": compiler,
            "server_threads": server_threads,
            "parallel_bench_processes": parallel_bench_processes,
            "runs": len(items),
            "elapsed_s_stats": compute_stats(elapsed_values),
            "elapsed_ms_stats": compute_stats(elapsed_ms_values),
            "energy_j_raw_stats": compute_stats(energy_raw_values),
            "idle_energy_j_estimated_stats": compute_stats(idle_estimated_values),
            "energy_j_stats": compute_stats(energy_values),
            "throughput_mib_s_stats": compute_stats(throughput_values),
            "downloads_per_process_stats": compute_stats(downloads_values),
            "total_iterations_stats": compute_stats(iterations_values),
            "success_stats": compute_stats(success_values),
            "failed_stats": compute_stats(failed_values),
        })

    return summary


# =========================
# CSV / TABLE FORMATTING
# =========================
def write_csv(results):
    fieldnames = [
        "compiler",
        "port",
        "server_threads",
        "parallel_bench_processes",
        "repetition",
        "success",
        "failed",
        "total_iterations",
        "downloads_per_process",
        "elapsed_s",
        "elapsed_ms",
        "idle_power_w",
        "energy_j_raw",
        "idle_energy_j_estimated",
        "energy_j",
        "real_transferred_bytes",
        "throughput_mib_s",
    ]

    with open(CSV_RESULTS, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for row in results:
            writer.writerow({k: row.get(k) for k in fieldnames})



def fmt(v, decimals=6):
    if v is None:
        return "-"
    if isinstance(v, int):
        return str(v)
    return f"{v:.{decimals}f}"



def wrap_text(text, width):
    if text is None:
        return "-"
    if not isinstance(text, str):
        text = str(text)
    parts = []
    for line in text.splitlines():
        wrapped = textwrap.wrap(line, width=width, break_long_words=False, replace_whitespace=False)
        if wrapped:
            parts.extend(wrapped)
        else:
            parts.append("")
    return "\n".join(parts)



def short_stats_cell(stats, wrap_width=TABLE_WRAP_MAIN):
    if not stats:
        return "-"
    return wrap_text(
        (
            f"Mean: {fmt(stats['mean'], 4)}\n"
            f"Median: {fmt(stats['median'], 4)}\n"
            f"P95: {fmt(stats['p95'], 4)}\n"
            f"Min: {fmt(stats['min'], 4)}\n"
            f"Max: {fmt(stats['max'], 4)}"
        ),
        wrap_width,
    )



def stats_summary_line(stats, unit=""):
    if not stats:
        return "-"
    suffix = f" {unit}" if unit else ""
    return (
        f"Mean {fmt(stats['mean'], 4)}{suffix}; "
        f"Median {fmt(stats['median'], 4)}{suffix}; "
        f"P95 {fmt(stats['p95'], 4)}{suffix}; "
        f"Min {fmt(stats['min'], 4)}{suffix}; "
        f"Max {fmt(stats['max'], 4)}{suffix}"
    )


# =========================
# PLOTS
# =========================
def make_per_run_scatter_with_mean(results, compiler, metric_key, metric_label, output_name):
    plt.figure(figsize=(11, 7))

    for server_threads in SERVER_THREADS:
        rows = [
            r for r in results
            if r["compiler"] == compiler and r["server_threads"] == server_threads
        ]
        rows.sort(key=lambda x: (x["parallel_bench_processes"], x["repetition"]))

        if not rows:
            continue

        x_all = [r["parallel_bench_processes"] for r in rows]
        y_all = [r[metric_key] for r in rows]
        plt.scatter(x_all, y_all, alpha=0.55, s=28, label=f"{server_threads} server thread(s) - runs")

        mean_rows = []
        for bench_count in MACRO_BENCH_CASES:
            selected = [r[metric_key] for r in rows if r["parallel_bench_processes"] == bench_count]
            if selected:
                mean_rows.append((bench_count, statistics.mean(selected)))

        if mean_rows:
            x_mean = [x for x, _ in mean_rows]
            y_mean = [y for _, y in mean_rows]
            plt.plot(x_mean, y_mean, marker="o", linewidth=2.2, label=f"{server_threads} server thread(s) - mean")

    plt.xlabel("Number of parallel benchmark client processes")
    plt.ylabel(metric_label)
    plt.title(f"{metric_label} for {compiler.upper()} builds")
    plt.xticks(MACRO_BENCH_CASES)
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / output_name)
    plt.close()



def make_compiler_comparison_plot(summary, server_threads, metric_stats_key, metric_label, output_name):
    plt.figure(figsize=(11, 7))

    for compiler in COMPILERS:
        rows = [
            r for r in summary
            if r["compiler"] == compiler and r["server_threads"] == server_threads
        ]
        rows.sort(key=lambda x: x["parallel_bench_processes"])

        x = [r["parallel_bench_processes"] for r in rows if r.get(metric_stats_key)]
        y = [r[metric_stats_key]["mean"] for r in rows if r.get(metric_stats_key)]

        if x and y:
            plt.plot(x, y, marker="o", linewidth=2.2, label=f"{compiler.upper()} - {server_threads} server thread(s)")

    plt.xlabel("Number of parallel benchmark client processes")
    plt.ylabel(metric_label)
    plt.title(f"{metric_label}: GCC vs Clang with {server_threads} server thread(s)")
    plt.xticks(MACRO_BENCH_CASES)
    plt.grid(True, alpha=0.35)
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / output_name)
    plt.close()



def generate_plots(results, summary):
    for compiler in COMPILERS:
        make_per_run_scatter_with_mean(
            results,
            compiler,
            "elapsed_ms",
            "Total execution time (milliseconds)",
            f"elapsed_time_{compiler}.png"
        )

        make_per_run_scatter_with_mean(
            results,
            compiler,
            "energy_j_raw",
            "Raw energy consumption (joules)",
            f"raw_energy_{compiler}.png"
        )

        make_per_run_scatter_with_mean(
            results,
            compiler,
            "energy_j",
            "Net energy consumption (joules)",
            f"net_energy_{compiler}.png"
        )

        make_per_run_scatter_with_mean(
            results,
            compiler,
            "throughput_mib_s",
            "Aggregate throughput (MiB/s)",
            f"throughput_{compiler}.png"
        )

        make_per_run_scatter_with_mean(
            results,
            compiler,
            "downloads_per_process",
            "Completed downloads per benchmark process",
            f"downloads_per_process_{compiler}.png"
        )

    for server_threads in SERVER_THREADS:
        make_compiler_comparison_plot(
            summary,
            server_threads,
            "elapsed_ms_stats",
            "Mean total execution time (milliseconds)",
            f"comparison_elapsed_threads_{server_threads}.png"
        )

        make_compiler_comparison_plot(
            summary,
            server_threads,
            "energy_j_stats",
            "Mean net energy consumption (joules)",
            f"comparison_net_energy_threads_{server_threads}.png"
        )

        make_compiler_comparison_plot(
            summary,
            server_threads,
            "throughput_mib_s_stats",
            "Mean aggregate throughput (MiB/s)",
            f"comparison_throughput_threads_{server_threads}.png"
        )


# =========================
# PDF HELPERS
# =========================
def add_text_page(pdf, title, lines, fontsize=11):
    fig = plt.figure(figsize=(8.27, 11.69))
    fig.clf()
    plt.axis("off")

    y = 0.97
    plt.figtext(0.05, y, title, fontsize=16, fontweight="bold", ha="left", va="top")
    y -= 0.04

    for line in lines:
        plt.figtext(0.05, y, line, fontsize=fontsize, ha="left", va="top", family="monospace")
        y -= 0.025
        if y < 0.05:
            pdf.savefig(fig, bbox_inches="tight")
            plt.close(fig)
            fig = plt.figure(figsize=(8.27, 11.69))
            fig.clf()
            plt.axis("off")
            y = 0.97

    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)



def add_table_page(
    pdf,
    title,
    columns,
    rows,
    fontsize=TABLE_FONT_SIZE,
    scale_y=TABLE_VERTICAL_SCALE,
    col_widths=None
):
    fig, ax = plt.subplots(figsize=(25, 14.4))
    ax.axis("off")
    ax.set_title(title, fontsize=16, fontweight="bold", pad=18)

    table = ax.table(
        cellText=rows,
        colLabels=columns,
        loc="center",
        cellLoc="left",
        colLoc="center",
        colWidths=col_widths,
    )

    table.auto_set_font_size(False)
    table.set_fontsize(fontsize)
    table.scale(1, scale_y)

    for (row, col), cell in table.get_celld().items():
        cell.set_linewidth(0.8)
        cell.get_text().set_wrap(True)

        if row == 0:
            cell.set_text_props(
                weight="bold",
                fontsize=TABLE_HEADER_FONT_SIZE,
                ha="center",
                va="center",
            )
            cell.set_height(cell.get_height() * 1.35)
        else:
            cell.set_text_props(va="center", ha="left")
            cell.set_height(cell.get_height() * 1.10)

    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)



def add_image_page(pdf, image_path, title):
    fig = plt.figure(figsize=(8.27, 11.69))
    plt.axis("off")
    img = plt.imread(image_path)
    plt.imshow(img)
    plt.title(title, fontsize=14)
    pdf.savefig(fig, bbox_inches="tight")
    plt.close(fig)


# =========================
# TABLE BUILDERS
# =========================
def build_summary_table_rows(summary):
    rows = []

    for item in summary:
        rows.append([
            item["compiler"].upper(),
            str(item["server_threads"]),
            str(item["parallel_bench_processes"]),
            str(item["runs"]),
            short_stats_cell(item["elapsed_ms_stats"], TABLE_WRAP_MAIN),
            short_stats_cell(item["energy_j_stats"], TABLE_WRAP_MAIN),
            short_stats_cell(item["throughput_mib_s_stats"], TABLE_WRAP_MAIN),
            short_stats_cell(item["downloads_per_process_stats"], TABLE_WRAP_MAIN),
        ])

    return rows



def build_reliability_table_rows(summary):
    rows = []

    for item in summary:
        rows.append([
            item["compiler"].upper(),
            str(item["server_threads"]),
            str(item["parallel_bench_processes"]),
            short_stats_cell(item["total_iterations_stats"], TABLE_WRAP_MAIN),
            short_stats_cell(item["success_stats"], TABLE_WRAP_MAIN),
            short_stats_cell(item["failed_stats"], TABLE_WRAP_MAIN),
        ])

    return rows



def build_raw_results_rows(results):
    rows = []

    for item in results:
        rows.append([
            item["compiler"].upper(),
            str(item["server_threads"]),
            str(item["parallel_bench_processes"]),
            str(item["repetition"]),
            fmt(item.get("elapsed_ms", item["elapsed_s"] * 1000.0), 4),
            fmt(item["energy_j_raw"], 4),
            fmt(item["energy_j"], 4),
            fmt(item["throughput_mib_s"], 4),
            str(item["success"]),
            str(item["failed"]),
        ])

    return rows



def build_comparison_table_rows(summary, server_threads):
    rows = []

    for bench_count in MACRO_BENCH_CASES:
        gcc_row = next(
            (x for x in summary if x["compiler"] == "gcc" and x["server_threads"] == server_threads and x["parallel_bench_processes"] == bench_count),
            None,
        )
        clang_row = next(
            (x for x in summary if x["compiler"] == "clang" and x["server_threads"] == server_threads and x["parallel_bench_processes"] == bench_count),
            None,
        )

        rows.append([
            str(bench_count),
            wrap_text(stats_summary_line(gcc_row["elapsed_ms_stats"], "ms") if gcc_row else "-", TABLE_WRAP_COMPARISON),
            wrap_text(stats_summary_line(clang_row["elapsed_ms_stats"], "ms") if clang_row else "-", TABLE_WRAP_COMPARISON),
            wrap_text(stats_summary_line(gcc_row["energy_j_stats"], "J") if gcc_row else "-", TABLE_WRAP_COMPARISON),
            wrap_text(stats_summary_line(clang_row["energy_j_stats"], "J") if clang_row else "-", TABLE_WRAP_COMPARISON),
            wrap_text(stats_summary_line(gcc_row["throughput_mib_s_stats"], "MiB/s") if gcc_row else "-", TABLE_WRAP_COMPARISON),
            wrap_text(stats_summary_line(clang_row["throughput_mib_s_stats"], "MiB/s") if clang_row else "-", TABLE_WRAP_COMPARISON),
        ])

    return rows


# =========================
# PDF REPORTS
# =========================
def generate_main_pdf_report(final_data, summary, results, output_path, include_raw_results):
    with PdfPages(output_path) as pdf:
        cover_lines = [
            "Main benchmark report",
            f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "",
            f"Served file: {final_data['file_served']}",
            f"Host: {final_data['host']}",
            f"Compilers: {', '.join(final_data['compilers']).upper()}",
            f"Ports: {final_data['ports']}",
            f"Parallel client cases: {final_data['cases']}",
            f"Server thread counts: {final_data['server_threads']}",
            f"Repetitions per case: {final_data['repetitions']}",
            f"Idle power baseline: {IDLE_POWER_W:.6f} W",
            f"Case cooldown: {CASE_COOLDOWN_SECONDS} seconds",
            f"Raw results included: {'yes' if include_raw_results else 'no'}",
        ]
        add_text_page(pdf, "Benchmark report", cover_lines, fontsize=11)

        summary_columns = [
            "Compiler",
            "Server\nthreads",
            "Parallel\nbenchmark\nclients",
            "Runs",
            "Execution time\nsummary (ms)",
            "Net energy\nsummary",
            "Throughput\nsummary",
            "Downloads per\nprocess summary",
        ]
        summary_rows = build_summary_table_rows(summary)

        chunk_size = 3
        summary_col_widths = [0.08, 0.08, 0.09, 0.06, 0.17, 0.17, 0.17, 0.18]
        for i in range(0, len(summary_rows), chunk_size):
            add_table_page(
                pdf,
                f"Summary table (part {i // chunk_size + 1})",
                summary_columns,
                summary_rows[i:i + chunk_size],
                fontsize=8,
                scale_y=TABLE_VERTICAL_SCALE,
                col_widths=summary_col_widths,
            )

        reliability_columns = [
            "Compiler",
            "Server\nthreads",
            "Parallel\nbenchmark\nclients",
            "Total iterations\nsummary",
            "Successful\nprocesses summary",
            "Failed\nprocesses summary",
        ]
        reliability_rows = build_reliability_table_rows(summary)
        reliability_col_widths = [0.10, 0.09, 0.10, 0.24, 0.23, 0.24]
        for i in range(0, len(reliability_rows), chunk_size):
            add_table_page(
                pdf,
                f"Reliability table (part {i // chunk_size + 1})",
                reliability_columns,
                reliability_rows[i:i + chunk_size],
                fontsize=8,
                scale_y=TABLE_VERTICAL_SCALE,
                col_widths=reliability_col_widths,
            )

        plot_titles = {
            "elapsed_time": "Total execution time",
            "raw_energy": "Raw energy consumption",
            "net_energy": "Net energy consumption",
            "throughput": "Aggregate throughput",
            "downloads_per_process": "Completed downloads per benchmark process",
        }

        for compiler in final_data["compilers"]:
            compiler_lines = [
                f"Compiler-focused report section for {compiler.upper()}.",
                "",
            ]
            compiler_rows = [x for x in summary if x["compiler"] == compiler]
            for row in compiler_rows:
                compiler_lines.append(
                    f"Server threads = {row['server_threads']}, parallel clients = {row['parallel_bench_processes']}: "
                    f"time -> {stats_summary_line(row['elapsed_ms_stats'], 'ms')} | "
                    f"net energy -> {stats_summary_line(row['energy_j_stats'], 'J')} | "
                    f"throughput -> {stats_summary_line(row['throughput_mib_s_stats'], 'MiB/s')}"
                )
            add_text_page(pdf, f"{compiler.upper()} overview", compiler_lines, fontsize=9)

            for prefix, natural_title in plot_titles.items():
                image_path = PLOTS_DIR / f"{prefix}_{compiler}.png"
                if image_path.exists():
                    add_image_page(pdf, image_path, f"{natural_title} for {compiler.upper()}")

        if include_raw_results:
            raw_columns = [
                "Compiler",
                "Server\nthreads",
                "Parallel\nbenchmark\nclients",
                "Rep.",
                "Execution\ntime (ms)",
                "Raw\nenergy (J)",
                "Net\nenergy (J)",
                "Throughput\n(MiB/s)",
                "Success",
                "Fail",
            ]
            raw_rows = build_raw_results_rows(results)
            raw_chunk = 14
            raw_col_widths = [0.08, 0.08, 0.09, 0.06, 0.11, 0.11, 0.11, 0.12, 0.07, 0.07]
            for i in range(0, len(raw_rows), raw_chunk):
                add_table_page(
                    pdf,
                    f"Raw execution results (part {i // raw_chunk + 1})",
                    raw_columns,
                    raw_rows[i:i + raw_chunk],
                    fontsize=8,
                    scale_y=TABLE_RAW_VERTICAL_SCALE,
                    col_widths=raw_col_widths,
                )



def generate_comparison_pdf_report(final_data, summary, results, output_path, include_raw_results):
    with PdfPages(output_path) as pdf:
        intro_lines = [
            "Compiler comparison report",
            f"Date: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "",
            "This report directly compares GCC and Clang under equivalent benchmark configurations.",
            "Each section groups the comparison by server thread count.",
            f"Raw results included: {'yes' if include_raw_results else 'no'}",
            "",
            f"Served file: {final_data['file_served']}",
            f"Parallel client cases: {final_data['cases']}",
            f"Server thread counts: {final_data['server_threads']}",
            f"Repetitions per case: {final_data['repetitions']}",
        ]
        add_text_page(pdf, "Compiler comparison report", intro_lines, fontsize=11)

        comparison_col_widths = [0.08, 0.15, 0.15, 0.15, 0.15, 0.16, 0.16]
        for server_threads in SERVER_THREADS:
            table_columns = [
                "Parallel\nbenchmark\nclients",
                "GCC execution\ntime summary (ms)",
                "Clang execution\ntime summary (ms)",
                "GCC net energy\nsummary",
                "Clang net energy\nsummary",
                "GCC throughput\nsummary",
                "Clang throughput\nsummary",
            ]
            table_rows = build_comparison_table_rows(summary, server_threads)
            add_table_page(
                pdf,
                f"GCC vs Clang comparison for {server_threads} server thread(s)",
                table_columns,
                table_rows,
                fontsize=8,
                scale_y=TABLE_COMPARISON_VERTICAL_SCALE,
                col_widths=comparison_col_widths,
            )

            comparison_images = [
                (PLOTS_DIR / f"comparison_elapsed_threads_{server_threads}.png", "Mean total execution time"),
                (PLOTS_DIR / f"comparison_net_energy_threads_{server_threads}.png", "Mean net energy consumption"),
                (PLOTS_DIR / f"comparison_throughput_threads_{server_threads}.png", "Mean aggregate throughput"),
            ]

            for image_path, title in comparison_images:
                if image_path.exists():
                    add_image_page(
                        pdf,
                        image_path,
                        f"{title}: GCC vs Clang with {server_threads} server thread(s)"
                    )

        if include_raw_results:
            comparison_raw_rows = []
            for item in results:
                comparison_raw_rows.append([
                    item["compiler"].upper(),
                    str(item["server_threads"]),
                    str(item["parallel_bench_processes"]),
                    str(item["repetition"]),
                    fmt(item.get("elapsed_ms", item["elapsed_s"] * 1000.0), 4),
                    fmt(item["energy_j"], 4),
                    fmt(item["throughput_mib_s"], 4),
                ])

            comparison_raw_columns = [
                "Compiler",
                "Server\nthreads",
                "Parallel\nclients",
                "Rep.",
                "Execution\ntime (ms)",
                "Net\nenergy (J)",
                "Throughput\n(MiB/s)",
            ]
            comparison_raw_col_widths = [0.10, 0.09, 0.10, 0.06, 0.13, 0.13, 0.14]
            raw_chunk = 18
            for i in range(0, len(comparison_raw_rows), raw_chunk):
                add_table_page(
                    pdf,
                    f"Comparison raw results (part {i // raw_chunk + 1})",
                    comparison_raw_columns,
                    comparison_raw_rows[i:i + raw_chunk],
                    fontsize=8,
                    scale_y=TABLE_RAW_VERTICAL_SCALE,
                    col_widths=comparison_raw_col_widths,
                )


# =========================
# CONSOLE SUMMARY
# =========================
def print_console_summary(summary):
    print("")
    print("==== STATISTICAL SUMMARY ====")

    for item in summary:
        compiler = item["compiler"]
        server_threads = item["server_threads"]
        benches = item["parallel_bench_processes"]

        elapsed = item["elapsed_ms_stats"]
        energy_raw = item["energy_j_raw_stats"]
        energy_net = item["energy_j_stats"]
        throughput = item["throughput_mib_s_stats"]

        print(
            f"[{compiler}] server_threads={server_threads} "
            f"parallel_bench_processes={benches}"
        )
        print(
            f"  time_ms: mean={elapsed['mean']:.6f} "
            f"median={elapsed['median']:.6f} "
            f"min={elapsed['min']:.6f} max={elapsed['max']:.6f}"
        )
        print(
            f"  raw_energy_j: mean={energy_raw['mean']:.6f} "
            f"median={energy_raw['median']:.6f} "
            f"min={energy_raw['min']:.6f} max={energy_raw['max']:.6f}"
        )
        print(
            f"  net_energy_j: mean={energy_net['mean']:.6f} "
            f"median={energy_net['median']:.6f} "
            f"min={energy_net['min']:.6f} max={energy_net['max']:.6f}"
        )
        print(
            f"  throughput_mib_s: mean={throughput['mean']:.6f} "
            f"median={throughput['median']:.6f} "
            f"min={throughput['min']:.6f} max={throughput['max']:.6f}"
        )
        print("")


# =========================
# MAIN
# =========================
def _run_one_scenario():
    ensure_results_dir()

    all_results = []

    try:
        for compiler in COMPILERS:
            for server_threads in SERVER_THREADS:
                results = run_campaign_for_compiler_and_threads(compiler, server_threads)
                all_results.extend(results)
    except KeyboardInterrupt:
        log("Interrupted by user.")
        raise

    final = {
        "file_served": FILE_TO_SERVE,
        "host": HOST,
        "ports": PORTS,
        "cases": MACRO_BENCH_CASES,
        "server_threads": SERVER_THREADS,
        "repetitions": MACRO_REPETITIONS,
        "compilers": COMPILERS,
        "build_dirs": BUILD_DIRS,
        "idle_baseline_json": str(IDLE_BASELINE_JSON),
        "idle_power_w": IDLE_POWER_W,
        "results": all_results
    }

    with open(FINAL_RESULTS, "w") as f:
        json.dump(final, f, indent=4)

    summary = summarize_results(all_results)

    with open(SUMMARY_RESULTS, "w") as f:
        json.dump({
            "file_served": FILE_TO_SERVE,
            "host": HOST,
            "ports": PORTS,
            "cases": MACRO_BENCH_CASES,
            "server_threads": SERVER_THREADS,
            "repetitions": MACRO_REPETITIONS,
            "compilers": COMPILERS,
            "build_dirs": BUILD_DIRS,
            "idle_baseline_json": str(IDLE_BASELINE_JSON),
            "idle_power_w": IDLE_POWER_W,
            "summary": summary
        }, f, indent=4)

    write_csv(all_results)

    if GENERATE_PLOTS and not DRY_RUN:
        generate_plots(all_results, summary)

    if GENERATE_PDF and not DRY_RUN:
        generate_main_pdf_report(final, summary, all_results, MAIN_PDF_WITH_RAW, include_raw_results=True)
        generate_main_pdf_report(final, summary, all_results, MAIN_PDF_NO_RAW, include_raw_results=False)

    if GENERATE_COMPARISON_PDF and not DRY_RUN:
        generate_comparison_pdf_report(final, summary, all_results, COMPARISON_PDF_WITH_RAW, include_raw_results=True)
        generate_comparison_pdf_report(final, summary, all_results, COMPARISON_PDF_NO_RAW, include_raw_results=False)

    if PRINT_FINAL_SUMMARY:
        print_console_summary(summary)

    log("Done.")
    log(f"Raw JSON: {FINAL_RESULTS}")
    log(f"Summary JSON: {SUMMARY_RESULTS}")
    log(f"CSV: {CSV_RESULTS}")
    log(f"Main PDF report with raw results: {MAIN_PDF_WITH_RAW}")
    log(f"Main PDF report without raw results: {MAIN_PDF_NO_RAW}")
    log(f"Comparison PDF report with raw results: {COMPARISON_PDF_WITH_RAW}")
    log(f"Comparison PDF report without raw results: {COMPARISON_PDF_NO_RAW}")
    log(f"Plots directory: {PLOTS_DIR}")
    log(f"Logs directory: {LOGS_DIR}")


def main():
    for name in active_scenarios():
        _activate_scenario(name)
        if is_done(str(RESULTS_DIR)):
            log(f"[{name}] already completed, skipping (resume)")
            continue
        log(f"=== Scenario: {name} (server={SERVER_DIR}, bench={BENCH_NAME}) ===")
        _run_one_scenario()
        mark_done(str(RESULTS_DIR))


if __name__ == "__main__":
    main()