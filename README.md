# Global Build and Benchmark Automation

This repository groups several experimental file-transfer implementations in **C++23**. The goal is to compare them under a common methodology in terms of:

- execution time
- scalability
- throughput
- energy consumption
- behavior under concurrency
- compiler-dependent differences
- single-threaded and multi-threaded server configurations

Each transport subproject contains its own implementation of:

- a TCP server
- a TCP client
- a Google Benchmark-based benchmark binary
- local automation scripts
- a project-specific `README.md`

In addition to those per-project READMEs, the repository root provides **global automation scripts** to build and run the whole benchmark suite from one place.

---

## Repository overview

The repository is organized as a collection of transport-specific benchmark projects, for example:

- `asio`
- `taps-asio`
- `async-berkeley`
- `bsd-sockets`
- `capy-corosio`

Each subproject may contain:

- its own `build_release.sh`
- its own `scripts/run_bench.py`
- its own `results/` directory
- its own compiler and dependency configuration

The global scripts do **not** replace that internal logic. They coordinate it.

---

## Purpose of the global scripts

At repository root, the workflow is centered around two scripts:

- `build.sh`: builds every transport project by delegating to each subproject's own `build_release.sh`
- `run.sh`: runs every benchmark campaign by delegating to each subproject's own `scripts/run_bench.py`

This makes it possible to:

- launch large build campaigns from one entry point
- run the full benchmark suite without entering each subproject manually
- preserve the internal build and run logic of each transport implementation
- automate the full thesis benchmarking workflow at repository level

---

## Expected subprojects

The global scripts are designed to iterate over these directories when present:

- `asio`
- `taps-asio`
- `async-berkeley`
- `bsd-sockets`
- `capy-corosio`

If a directory does not exist, or if the expected script is missing, it is skipped and reported.

---

## Global build script

### Name

```bash
sudo ./build.sh
```

### What it does

The global build script:

- assumes it is executed from the repository root
- checks that the basic required tools are available
- ensures Python dependencies needed by the reporting pipeline are installed
- enters each transport subproject
- runs its `build_release.sh` if present
- reports progress for the global build process

Each subproject remains fully responsible for:

- how it is compiled
- which compilers it uses
- which flags it enables
- which external libraries it requires
- how special cases such as TAPS or async-berkeley are resolved

The root-level build script only acts as a coordinator.

---

## Python dependencies used by the global workflow

Some benchmark pipelines generate figures and merged PDF reports. For that reason, the global build script may ensure the following Python packages are available:

- `matplotlib`
- `pypdf`

### Installation strategy

The script attempts to install missing dependencies in this order:

1. system packages with `apt-get`
2. fallback installation with `python3 -m pip install --user`

Typical Ubuntu commands are:

```bash
sudo apt-get update
sudo apt-get install -y python3-matplotlib python3-pypdf python3-pip
```

---

## Global benchmark execution script

### Name

```bash
sudo ./run.sh
```

### What it does

The global run script:

- enters each configured subproject
- looks for its `scripts/run_bench.py`
- launches the benchmark campaign if the script exists
- lets each project generate its own raw data, summaries, CSV files, plots, and PDF reports
- collects the resulting artifacts into a global results tree
- builds a repository-wide master summary
- optionally merges categorized PDF reports

This means the detailed benchmark behavior still belongs to each individual subproject, while the root script provides a unified orchestration layer.

---

## Network-realism sweep (D7): `netem/run_rtt_sweep.sh`

### Name

```bash
./netem/run_rtt_sweep.sh
```

(`sudo` is not required as a prefix if `./build.sh` has already been run at least
once — it installs a NOPASSWD sudo rule scoped specifically to the `tc` binary, via
`/etc/sudoers.d/tc-netem`. Running it under `sudo` directly also works, same as
`build.sh`/`run.sh`.)

### Why it exists

Every other campaign above measures over loopback, with no simulated network
latency or loss. That is not just an incompleteness: a real debugging session
(2026-09-14, see `design/tls_experiment_notes.md` D7) found a small TLS-scenario
timing delta between two implementations at RTT=0 that **reversed sign** once
~1.7 ms of round-trip latency was simulated. A loopback-only number would have
reported that artifact as if it were the actual finding. This script exists so
that check is a one-command rerun, not a from-scratch investigation, whenever a
loopback-only result looks surprising -- and, more generally, so every scenario
(not just TLS) can be measured under realistic RTT and packet loss, answering
reviewer R2-c's "loopback-only is not representative" for the whole benchmark
matrix, not one corner of it.

### What it does

Client and server traffic is carried over a Linux network namespace + veth pair
(not shaped directly on `lo`): the server runs inside a dedicated namespace, the
client (and the rest of this harness) stays in the root namespace and connects to
the namespace's veth IP. This gives each direction its own genuinely separate
interface/queue, rather than sharing loopback's single egress queue.

Once, at the start: the netns+veth topology is created (idempotent -- safe to
rerun after a crash). Then, for each (RTT, loss) point in the
`NETEM_RTTS_MS` x `NETEM_LOSS_PCT` grid:

1. shapes both veth endpoints with `tc qdisc ... netem delay ... rate ... loss
   gemodel ...` to the target RTT and average loss rate (a bandwidth cap is
   always paired with the delay -- without one, the link's unbounded bandwidth
   overflows netem's queue on a bulk transfer and produces real TCP
   retransmission stalls, not a clean latency simulation; loss uses the Simple
   Gilbert two-state model -- bursty, not independent-per-packet -- parameterised
   by target average loss rate and mean burst length in packets, see
   `netem_common.sh` for the derivation and citations)
2. verifies the RTT shaping actually took effect with `ping` across the veth
   link, aborting that point rather than silently recording a mislabelled result
3. runs `NETEM_SCENARIOS` for each project in `NETEM_PROJECTS`, via each
   project's own `scripts/run_bench.py` (`RUN_SCENARIOS` is how scenario
   selection already works everywhere else in this repo); `NETEM_SERVER_HOST`/
   `NETEM_SERVER_NETNS`, exported by `netem_common.sh`, tell each `run_bench.py`
   to launch the server inside the namespace and point the client at its veth IP
4. moves each project's `results/<scenario>/` into
   `results/<scenario>__netem_rtt_<R>ms_loss_<L>pct/` -- a pre-existing
   (non-netem) `results/<scenario>` is always moved aside with a timestamp
   first, never overwritten or deleted

Once, at the end (also on any error/interrupt, via a trap -- an interrupted sweep
never leaves the namespace or a qdisc behind): the topology is torn down.

### Configuration

```bash
NETEM_RTTS_MS="0 1 5 10 20 50"       # target RTTs in ms (default shown)
NETEM_LOSS_PCT="0 0.1 1 5"           # target average loss rates in % (default shown)
NETEM_MEAN_BURST_PKTS=3              # Simple Gilbert: mean consecutive packets per loss event
NETEM_SCENARIOS="streaming whole_object blocks tls tls_framed udp_k64 udp_k1400"  # default: every scenario
NETEM_PROJECTS="asio taps-asio async-berkeley bsd-sockets capy-corosio"
NETEM_RATE_MBIT=1000                 # bandwidth cap paired with the delay
NETEM_LIMIT_PKTS=50000               # netem queue depth
NETEM_RTT_TOLERANCE_MS=2             # how far measured RTT may drift from target
DRY_RUN=1                            # print the plan, touch no qdisc/netns, run nothing
```

Typical usage:

```bash
./netem/run_rtt_sweep.sh
NETEM_RTTS_MS="0 1 10" NETEM_LOSS_PCT="0 1" NETEM_SCENARIOS="tls" ./netem/run_rtt_sweep.sh
```

Per-point TCP environment snapshots (congestion control, `tcp_rmem`/`tcp_wmem`,
offload flags) are written to
`results_netem/tcp_env_rtt_<R>ms_loss_<L>pct.txt`.

The default grid is the full scenario matrix x 6 RTT points x 4 loss points (24
grid points); this multiplies total campaign runtime accordingly. Narrow
`NETEM_SCENARIOS`/`NETEM_RTTS_MS`/`NETEM_LOSS_PCT` to iterate faster.

---

## Recommended workflow

From repository root:

```bash
sudo ./build.sh
sudo ./run.sh
```

With this workflow:

1. all available implementations are built
2. all configured benchmark campaigns are executed
3. each subproject produces its own local results
4. the repository root collects those results into a unified global structure
5. cross-library comparisons become easier to inspect

---

## Global results layout

The current root-level automation is intended to store consolidated outputs under a structure like:

```text
global_results/
├── raw/
├── summaries/
├── csv/
├── reports/
│   ├── main_with_raw/
│   ├── main_without_raw/
│   ├── comparison_with_raw/
│   ├── comparison_without_raw/
│   ├── per_library/
│   └── merged/
├── plots/
├── per_project/
├── logs/
└── manifests/
```

### Meaning of the main folders

- `raw/`: collected raw JSON data from all subprojects
- `summaries/`: collected per-library summary JSON files plus the global master summary
- `csv/`: collected per-library CSV files plus the global master CSV
- `reports/`: categorized PDF reports produced by each transport implementation
- `plots/`: copied plot outputs from individual projects when available
- `per_project/`: a clean per-library mirror of collected outputs
- `logs/`: global orchestration logs
- `manifests/`: run configuration and execution metadata

This structure is meant to make the full campaign easier to inspect globally, while still preserving per-project separation.

---

## Global master summary

After the full execution, the repository-level workflow can generate a consolidated master table from the collected `*_summary.json` files.

Typical outputs:

- `global_results/summaries/master_summary.json`
- `global_results/csv/master_summary.csv`

These master files are intended to support comparison across:

- libraries
- compilers
- server thread counts
- numbers of parallel benchmark clients

They can also be extended to derive higher-level metrics such as:

- throughput per joule
- best library per case
- average behavior by library
- per-case winners for latency, energy, throughput, or efficiency

---

## Merged global reports

The global PDF merge step can combine categorized PDF reports into grouped outputs, for example:

- merged main reports with raw tables
- merged main reports without raw tables
- merged comparison reports with raw tables
- merged comparison reports without raw tables
- one combined global PDF containing all available report categories

This makes the final inspection of results much easier than opening each project report manually.

---

## Cooldown, cache trashing, and warmup phases

The global run script may expose environment-controlled behavior such as:

- cooldown before each project
- cooldown after each project
- best-effort cache trashing
- optional warmup phase
- optional randomized project order
- optional idle measurement at the beginning

This is useful when trying to reduce cross-project interference and improve the consistency of the measurements.

Examples of environment variables that may be supported:

```bash
SETTLE_SECONDS_BEFORE=20
SETTLE_SECONDS_AFTER=20
WARMUP_ENABLED=1
WARMUP_PROJECTS=1
CACHE_TRASH_ENABLED=1
CACHE_TRASH_SIZE_MB=2048
RANDOMIZE_ORDER=0
MERGE_PDFS_AT_END=1
MEASURE_IDLE_AT_START=1
```

A typical usage pattern is:

```bash
sudo SETTLE_SECONDS_BEFORE=20 SETTLE_SECONDS_AFTER=20 ./run.sh
```

---

## Permissions

If the root scripts do not have execution permissions:

```bash
chmod +x build.sh
chmod +x run.sh
```

---

## Relationship with the per-project READMEs

This root README does **not** replace the `README.md` file inside each transport implementation.

Its role is to complement them by explaining:

- how to build all projects from the repository root
- how to run all benchmark campaigns from the repository root
- how global result collection and cross-library comparison work
- how the root-level scripts fit into the full thesis workflow

For transport-specific details, the corresponding subproject README should always be consulted.

---

## Notes

- Each transport project still owns its own compiler setup, ports, server-thread settings, benchmark cases, and result-generation logic.
- The global scripts only coordinate the complete repository-level workflow.
- If project directories are renamed, the `PROJECT_DIRS` arrays in the root scripts must be updated accordingly.
- This design keeps transport implementations decoupled while still enabling repository-wide automation and comparison.

## Acknowledgements

Special thanks to **Jesús Martínez Cruz** and **José Carlos** for their work on the experimental C++ TAPS implementation and for providing the updated TAPS repository used in this benchmark campaign:

- <https://github.com/jmcruz-uma/taps_cpp/tree/main>

Their contribution made it possible to evaluate TAPS under the same raw TCP file-transfer methodology used for the rest of the C++ middleware implementations. The TAPS code used in this context should be understood as experimental and academic work intended to support reproducible evaluation and discussion within the C++ networking community.

---

## Author

**José Antonio García Montañez**