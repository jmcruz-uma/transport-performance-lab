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

## Setting up a fresh measurement machine, unattended

For a clean Ubuntu 24.04 machine that will run the full campaign (baseline +
D7 netem sweep) unattended while nobody is watching it:

```bash
git clone https://github.com/jmcruz-uma/transport-performance-lab.git
cd transport-performance-lab
sudo ./preflight.sh      # ~1-2 minutes: installs + verifies every prerequisite,
                          # fails LOUDLY now instead of silently two days from now
sudo ./tune_machine.sh apply   # optional but recommended: low-variance,
                                # RAPL-clean machine tuning -- see the section
                                # below for what it does and why; skip this
                                # line if you'd rather measure on a normally
                                # configured machine
sudo ./run_everything.sh # the one command: preflight -> build -> smoke test ->
                          # baseline campaign -> D7 netem RTT x loss sweep
```

Do **not** skip running `preflight.sh` on its own first. `run_everything.sh`
also runs it as its first stage, but running it standalone first means a
missing dependency is a 2-minute fix you see immediately, not something
discovered after `run_everything.sh` has already spent time on earlier stages.

`tune_machine.sh apply` belongs **between** `preflight.sh` and
`run_everything.sh`, run once, by hand -- never automatically, and never
after the campaign has already started (see the "Machine tuning" section
below for why it's a separate, deliberate step, and for `tune_machine.sh
restore` if you ever want the machine back to how it was).

### What `preflight.sh` checks

Installs (via `apt-get`, idempotent) and then verifies -- not just "is the
package installed" but "does the capability actually work":

- compilers: `gcc-14`/`g++-14`, `clang-18` (and that the bare `clang`/`clang++`
  commands resolve to the -18 toolchain, since every `build_release.sh` calls
  them by that name), `libc++-18-dev`/`libc++abi-18-dev`
- `libssl-dev` (every TLS/TLS-framed scenario needs it)
- `cmake` >= 3.20
- network namespaces + veth pairs + the `sch_netem` qdisc actually work
  (creates and tears down a throwaway test namespace -- this is what D7 needs)
- **RAPL is readable** (`/sys/class/powercap/intel-rapl:0/energy_uj`) -- this
  is the entire reason to run on real hardware instead of WSL2/a VM; every
  energy number in the campaign depends on it, so this check exists to fail
  loudly *before* a multi-day run rather than silently produce zeros
- network access to GitHub (every project `FetchContent`s its dependencies
  from there: standalone Asio, Google Benchmark, corosio, capy, taps_cpp)
- free disk space (15+ GiB recommended: 2 compilers x 5 projects x fetched
  sources, plus days of accumulated results/plots/PDFs)
- the Python modules the reporting pipeline needs (`matplotlib`, `pypdf`)

If anything fails, `preflight.sh` prints every failure at once (not just the
first one) with what's wrong and, where applicable, how to fix it, then exits
non-zero. Fix everything it reports before moving on. It's idempotent -- rerun
it as many times as needed.

### What `run_everything.sh` does

Five stages, each logged to `campaign_logs/<timestamp>/campaign.log` and
summarized in one line in `campaign_status.txt` at the repo root (so checking
in on a run that's been going for a day is `cat campaign_status.txt`, not
reading a huge log):

1. **preflight** -- reruns `preflight.sh`; aborts immediately if it fails
2. **build** -- `build.sh` (all 5 projects, both compilers)
3. **smoke test** -- one real transfer per project/compiler over loopback
   (the `streaming` scenario, no certs needed) as a fast sanity check that
   what was just built actually runs correctly, not just compiles. A failure
   here is logged prominently as a warning but does not stop the run -- by
   design, so a single flaky smoke-test iteration never throws away hours of
   otherwise-good build/campaign work; read the log if you see one
4. **baseline campaign** -- `run.sh`: every scenario, every project, loopback
   only (RTT=0, no netem) -- this is the "everything measured the way it
   always has been" pass
5. **D7 netem sweep** -- `netem/run_rtt_sweep.sh`: every scenario, every
   project, across the full RTT x loss grid (default: 6 RTT points x 4 loss
   points = 24 grid points, netns+veth topology) -- see the "Network-realism
   sweep (D7)" section above for what this measures and why

Any stage failing aborts the whole run (no point starting a multi-day sweep on
top of a broken build) and writes the failure -- which stage, and a pointer to
the full log -- to `campaign_status.txt`. Every stage is safe to rerun:
builds are incremental, `run.sh`/the netem sweep resume by scenario via their
own checkpoint files, and a pre-existing result is never overwritten (moved
aside with a timestamp instead) -- so after fixing whatever `preflight.sh` or
the log pointed at, just run `sudo ./run_everything.sh` again.

**Expect this to take a long time.** Stages 4 and 5 are the real campaign:
every scenario's own case/thread/compiler/repetition grid, multiplied by 24
grid points for stage 5 alone. Narrow `NETEM_RTTS_MS`/`NETEM_LOSS_PCT`/
`NETEM_SCENARIOS` (see the D7 section above) before running if a shorter first
pass is wanted; the defaults assume the machine can be left alone for
multiple days.

---

## Machine tuning for low-variance, RAPL-clean measurements: `tune_machine.sh`

### Name

```bash
sudo ./tune_machine.sh apply     # before run_everything.sh
sudo ./tune_machine.sh status    # read-only, no root needed
sudo ./tune_machine.sh restore   # undo everything, exactly
```

### Why it exists, and why it's separate from `run_everything.sh`

A normally-configured machine's CPU frequency scaling, Turbo Boost, and
hyperthreading all introduce run-to-run variance that is not just noise --
Mytkowicz et al., *"Producing Wrong Data Without Doing Anything Obviously
Wrong!"* (ASPLOS 2009), show that innocuous-looking environment differences
can bias which of two implementations looks faster. For this project that
risk is doubled: every energy number comes from RAPL, and Turbo Boost/governor
scaling change *power draw* unpredictably between runs, which contaminates
energy comparisons between arms even more directly than it contaminates
timing.

Applying this is a **deliberate methodological choice** (it trades
"representative of a normally configured machine" for "low-variance,
comparable across arms"), not something that should happen silently as a side
effect of running the campaign -- so it is a separate, explicit, one-time step
you run by hand before `run_everything.sh`, never invoked automatically.

### What it tunes (Tier 1 -- system-wide, no reboot, no changes to any
### experiment script)

- CPU governor -> `performance` (no frequency scaling between runs)
- Turbo Boost -> off (the single biggest source of power-draw variance)
- SMT/hyperthreading -> off (removes sibling-thread cache/execution-unit
  contention as a noise source)
- ASLR -> off, NMI watchdog -> off, Transparent Huge Pages -> `never`
  (standard variance reducers)
- swap -> off (nothing should ever page during a run)
- clocksource -> `tsc` if available (highest-resolution, lowest-overhead
  timekeeping)

**Deliberately out of scope (Tier 2):** CPU isolation (`isolcpus=`/
`nohz_full=`/`rcu_nocbs=` on the kernel command line) plus pinning the
server/client processes onto those isolated cores with `taskset`. That would
directly address the exact mechanism the 2026-09-14 `sched_switch`
investigation found (see `design/tls_experiment_notes.md` D7 -- a WSL2 pilot
found a small TAPS-vs-asio delta that traced entirely to a 770-vs-226
scheduling-event difference), but it needs a reboot (kernel command line) and
touches `run_bench.py` in all 5 projects (to actually launch under `taskset`),
so it was deliberately deferred rather than bundled into this pass.

### Every change is verified, not assumed

Each tunable is applied, then **read back** to confirm the value actually
took effect before this script reports it as `OK` -- a write to a sysfs file
can silently fail or partially fail (confirmed while testing this script:
`echo off > .../smt/control` returned a "Device or resource busy" error under
WSL2, since a hypervisor's virtual CPUs generally can't be hot-unplugged by
the guest the way SMT-off requires -- the script correctly reports this as
`FAIL` rather than claiming success; this should not happen on bare-metal
Ubuntu 24.04, which is the actual target). A tunable that fails to verify is
reported as `FAIL`, counted, and reflected in the exit code -- `apply`
finishing without printing any `FAIL` line is what "it worked" actually looks
like, not just "the script reached the end".

### Restoring is exact, not "sane defaults"

Before changing anything, `apply` snapshots the machine's *actual current*
value for every tunable it touches (not an assumed default) into
`.tune_machine_state` at the repo root. `restore` reads that file back and
puts every value back exactly as found, then deletes the state file. Because
of this, `apply` refuses to run again on top of an existing state file --
`restore` first, then `apply` again, if you need to re-run it.

---

## Every result, from every experiment, has the same shape

Every scenario -- `streaming`/`whole_object`/`blocks`/`tls`/`tls_framed`/
`udp_k64`/`udp_k1400`, under plain loopback (`run.sh`) or under any point of
the D7 netem RTT x loss grid (`netem/run_rtt_sweep.sh`) -- is produced by the
exact same code path in each project's `scripts/run_bench.py`. The netem
sweep changes *where the server and client connect* (a namespace's veth IP
instead of `127.0.0.1`) and *what `RUN_SCENARIOS`/env it's invoked with*; it
never touches the stats-computation or file-writing code. **The result files
are byte-for-byte the same schema whether you're looking at a loopback
baseline or an RTT=20ms/loss=1% netem grid point** -- only *where* they end
up on disk differs (see below). Nothing about interpreting one is different
from interpreting the other.

### What each per-(project, scenario) run produces, under `<project>/results/<label>/`

(`<label>` is the plain scenario name for a `run.sh` baseline, e.g.
`streaming`, or `<scenario>__netem_rtt_<R>ms_loss_<L>pct` for a D7 grid
point, e.g. `tls__netem_rtt_20ms_loss_1pct`.)

- `raw/micro_<compiler>_threads_<N>_<case>_<rep>.json` -- one file per
  individual repetition (Google Benchmark's raw output for that single run)
- `raw/macro_bench_results.json` -- every repetition of every
  (compiler, server_threads, case) combination in the scenario's grid,
  concatenated
- `raw/macro_bench_summary.json` -- the one to read first. Per (compiler,
  server_threads, `parallel_bench_processes` = concurrency case), summary
  statistics (`count`/`mean`/`median`/`stdev`/`min`/`max`/`p25`/`p50`/`p95`)
  over all repetitions of:
  - `elapsed_s` / `elapsed_ms` -- wall-clock time of the transfer
  - `energy_j_raw` -- raw RAPL energy delta over the transfer (0 on any
    machine without RAPL, e.g. WSL2 -- if you see all-zero energy after
    deploying, that is the tell that RAPL isn't being read, check
    `preflight.sh`'s RAPL check again)
  - `idle_energy_j_estimated` -- what the same elapsed time would have cost
    at the machine's measured idle power draw (see `measure_idle_energy.py`,
    `idle_baseline.json`)
  - `energy_j` -- the energy actually attributable to the transfer
    (`energy_j_raw` minus the idle estimate)
  - `throughput_mib_s`, `downloads_per_process`, `total_iterations`
  - `success` / `failed` counts
- `raw/macro_bench_results.csv` -- the same data as the results JSON, flattened to CSV
- `reports/macro_bench_report_{with,no}_raw.pdf`,
  `macro_bench_comparison_report_{with,no}_raw.pdf` -- per-project PDF
  reports for that one scenario/grid-point
- `plots/`, `logs/`, `checkpoints/` -- generated plots, server/bench stderr
  logs, and the `scenario_done.json` checkpoint that makes a scenario
  resumable/skippable on rerun

### Where the global (cross-library) view collects all of this

`run.sh` walks every `<project>/results/<label>/` directory it finds (there
had been a **real, since-fixed bug here**: the collection step used to look
for a single flat `results/raw/...` path that `run_bench.py` has never
actually written to -- every project always writes per-`<label>`, so the
global collection step silently found nothing from any real multi-scenario
campaign until this was caught and fixed on 2026-09-15, ahead of the
real-machine deployment) and mirrors everything under `global_results/`,
organized by `<label>` so results from different scenarios/grid-points are
never mixed together:

```text
global_results/
├── raw/<label>/<project>_raw.json
├── summaries/<label>/<project>_summary.json
├── summaries/master_summary__<label>.json      <- one master table PER label
├── csv/<label>/<project>_raw.csv
├── csv/master_summary__<label>.csv             <- one master CSV PER label
├── reports/
│   ├── main_with_raw/<label>/, main_without_raw/<label>/,
│   │   comparison_with_raw/<label>/, comparison_without_raw/<label>/,
│   │   per_library/<label>/               <- per-project PDFs, by label
│   ├── global/master__<label>.pdf         <- one master PDF PER label
│   └── merged/                             <- see "Merged global reports" below
├── plots/<label>/<project>/, plots/global/<label>/
├── per_project/<project>/<label>/          <- full detail, mirrors the project's own results/<label>/
├── logs/
└── manifests/
```

**Why per-label, not one giant global table:** the master table groups rows
by (compiler, server_threads, concurrency case) and finds the fastest/
lowest-energy/highest-throughput row per group -- that comparison is only
meaningful between rows measuring the *same condition*. Mixing e.g. `asio`
under `streaming`/RTT=0 with `taps-asio` under `tls`/RTT=20ms/loss=1% into
one table would silently compare two different experiments as if they were
peers. One master table per `<label>` keeps every comparison apples-to-apples
while still collecting every single result -- nothing from any scenario or
grid point is dropped.

With the default D7 grid (6 RTT points x 4 loss points = 24, times 7
scenarios) plus the loopback baseline, expect on the order of 170+ labels
(fewer in practice: `async-berkeley` has no TLS scenarios, and some
combinations may be narrowed via `NETEM_SCENARIOS`/`NETEM_RTTS_MS`/
`NETEM_LOSS_PCT`). Each `master_summary__<label>.json`/`.csv`/PDF is the
starting point for comparing the 5 libraries under that one condition;
comparing *across* labels (e.g. "how does asio's `tls` delta from `streaming`
change as RTT grows") means reading multiple label files together -- there
is no single "does everything" master file, by design (see above).

### Master table columns

Each row in a `master_summary__<label>.json`/`.csv` is one
(library, compiler, server_threads, concurrency case), with every
`macro_bench_summary.json` stat flattened into columns
(`elapsed_ms_mean`, `elapsed_ms_p95`, `energy_j_mean`, `throughput_mib_s_mean`,
...) plus two derived columns: `throughput_per_joule_mean/median` and
`downloads_per_second_mean`. The accompanying PDF plots the same comparisons
graphically per case.

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

Special thanks to **Jesús Martínez** and **José Carlos Sequera** for their work on the experimental C++ TAPS implementation and for providing the updated TAPS repository used in this benchmark campaign:

- <https://github.com/jmcruz-uma/taps_cpp/tree/main>

Their contribution made it possible to evaluate TAPS under the same raw TCP file-transfer methodology used for the rest of the C++ middleware implementations. The TAPS code used in this context should be understood as experimental and academic work intended to support reproducible evaluation and discussion within the C++ networking community.

---

## Authors

**José Antonio García-Montañez**
**Jesús Martínez-Cruz**
