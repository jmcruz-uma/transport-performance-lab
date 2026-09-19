#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GLOBAL_RESULTS_DIR="$ROOT_DIR/global_results"

RAW_DIR="$GLOBAL_RESULTS_DIR/raw"
SUMMARIES_DIR="$GLOBAL_RESULTS_DIR/summaries"
CSV_DIR="$GLOBAL_RESULTS_DIR/csv"
REPORTS_DIR="$GLOBAL_RESULTS_DIR/reports"
REPORTS_MAIN_WITH_RAW_DIR="$REPORTS_DIR/main_with_raw"
REPORTS_MAIN_WITHOUT_RAW_DIR="$REPORTS_DIR/main_without_raw"
REPORTS_COMPARISON_WITH_RAW_DIR="$REPORTS_DIR/comparison_with_raw"
REPORTS_COMPARISON_WITHOUT_RAW_DIR="$REPORTS_DIR/comparison_without_raw"
REPORTS_PER_LIBRARY_DIR="$REPORTS_DIR/per_library"
MERGED_REPORTS_DIR="$REPORTS_DIR/merged"
GLOBAL_REPORTS_DIR="$REPORTS_DIR/global"

PLOTS_DIR="$GLOBAL_RESULTS_DIR/plots"
GLOBAL_PLOTS_DIR="$PLOTS_DIR/global"
PER_PROJECT_DIR="$GLOBAL_RESULTS_DIR/per_project"
LOGS_DIR="$GLOBAL_RESULTS_DIR/logs"
MANIFESTS_DIR="$GLOBAL_RESULTS_DIR/manifests"

SYSTEM_INFO_TXT="$GLOBAL_RESULTS_DIR/system_info.txt"
RUN_LOG="$LOGS_DIR/run_log.txt"
RUN_MANIFEST_JSON="$MANIFESTS_DIR/run_manifest.json"

PROJECT_DIRS=(
  "asio"
  "taps-asio"
  "async-berkeley"
  "bsd-sockets"
  "capy-corosio"
)

SETTLE_SECONDS_BEFORE="${SETTLE_SECONDS_BEFORE:-20}"
SETTLE_SECONDS_AFTER="${SETTLE_SECONDS_AFTER:-20}"
WARMUP_ENABLED="${WARMUP_ENABLED:-0}"
WARMUP_PROJECTS="${WARMUP_PROJECTS:-1}"
CACHE_TRASH_ENABLED="${CACHE_TRASH_ENABLED:-1}"
CACHE_TRASH_SIZE_MB="${CACHE_TRASH_SIZE_MB:-2048}"
RANDOMIZE_ORDER="${RANDOMIZE_ORDER:-0}"
MERGE_PDFS_AT_END="${MERGE_PDFS_AT_END:-1}"
MEASURE_IDLE_AT_START="${MEASURE_IDLE_AT_START:-1}"

log() {
  local msg="[$(date '+%H:%M:%S')] $*"
  echo "$msg"
  echo "$msg" >> "$RUN_LOG"
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

prepare_dirs() {
  mkdir -p \
    "$GLOBAL_RESULTS_DIR" \
    "$RAW_DIR" \
    "$SUMMARIES_DIR" \
    "$CSV_DIR" \
    "$REPORTS_DIR" \
    "$REPORTS_MAIN_WITH_RAW_DIR" \
    "$REPORTS_MAIN_WITHOUT_RAW_DIR" \
    "$REPORTS_COMPARISON_WITH_RAW_DIR" \
    "$REPORTS_COMPARISON_WITHOUT_RAW_DIR" \
    "$REPORTS_PER_LIBRARY_DIR" \
    "$MERGED_REPORTS_DIR" \
    "$GLOBAL_REPORTS_DIR" \
    "$PLOTS_DIR" \
    "$GLOBAL_PLOTS_DIR" \
    "$PER_PROJECT_DIR" \
    "$LOGS_DIR" \
    "$MANIFESTS_DIR"

  : > "$RUN_LOG"
}

save_manifest() {
  cat > "$RUN_MANIFEST_JSON" <<EOF
{
  "root_dir": "$ROOT_DIR",
  "global_results_dir": "$GLOBAL_RESULTS_DIR",
  "raw_dir": "$RAW_DIR",
  "summaries_dir": "$SUMMARIES_DIR",
  "csv_dir": "$CSV_DIR",
  "reports_dir": "$REPORTS_DIR",
  "global_reports_dir": "$GLOBAL_REPORTS_DIR",
  "merged_reports_dir": "$MERGED_REPORTS_DIR",
  "plots_dir": "$PLOTS_DIR",
  "global_plots_dir": "$GLOBAL_PLOTS_DIR",
  "per_project_dir": "$PER_PROJECT_DIR",
  "logs_dir": "$LOGS_DIR",
  "manifests_dir": "$MANIFESTS_DIR",
  "master_json_pattern": "$SUMMARIES_DIR/<label>/master_summary__<label>.json",
  "master_csv_pattern": "$CSV_DIR/master_summary__<label>.csv",
  "master_pdf_pattern": "$GLOBAL_REPORTS_DIR/master__<label>.pdf",
  "system_info_txt": "$SYSTEM_INFO_TXT",
  "run_log": "$RUN_LOG",
  "settle_seconds_before": $SETTLE_SECONDS_BEFORE,
  "settle_seconds_after": $SETTLE_SECONDS_AFTER,
  "warmup_enabled": $WARMUP_ENABLED,
  "warmup_projects": $WARMUP_PROJECTS,
  "cache_trash_enabled": $CACHE_TRASH_ENABLED,
  "cache_trash_size_mb": $CACHE_TRASH_SIZE_MB,
  "randomize_order": $RANDOMIZE_ORDER,
  "merge_pdfs_at_end": $MERGE_PDFS_AT_END,
  "measure_idle_at_start": $MEASURE_IDLE_AT_START
}
EOF
}

save_system_info() {
  {
    echo "==== BENCHMARK SYSTEM INFO ===="
    echo "Timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "==== uname -a ===="
    uname -a || true
    echo
    echo "==== hostnamectl ===="
    hostnamectl || true
    echo
    echo "==== lscpu ===="
    lscpu || true
    echo
    echo "==== free -h ===="
    free -h || true
    echo
    echo "==== lsblk ===="
    lsblk || true
    echo
    echo "==== governors ===="
    grep . /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true
    echo
    echo "==== /proc/cmdline ===="
    cat /proc/cmdline || true
    echo
    echo "==== env ===="
    env | sort
  } > "$SYSTEM_INFO_TXT"
}

measure_idle_if_available() {
  if [ "$MEASURE_IDLE_AT_START" != "1" ]; then
    log "Idle measurement disabled"
    return
  fi

  local idle_script="$ROOT_DIR/measure_idle_energy.py"

  if [ ! -f "$idle_script" ]; then
    log "measure_idle_energy.py not found at repository root; continuing without a new idle measurement"
    return
  fi

  log "Measuring system idle baseline..."
  python3 "$idle_script"
}

trash_caches_best_effort() {
  if [ "$CACHE_TRASH_ENABLED" != "1" ]; then
    return
  fi

  log "Cache trashing best-effort (${CACHE_TRASH_SIZE_MB} MB)..."

  python3 - <<PY
size_mb = int("${CACHE_TRASH_SIZE_MB}")
chunk = 1024 * 1024
buf = bytearray(chunk)
acc = 0

for i in range(size_mb):
    for j in range(0, len(buf), 4096):
        buf[j] = (i + j) & 0xFF
        acc ^= buf[j]

print(acc)
PY

  if [ -w /proc/sys/vm/drop_caches ]; then
    log "Dropping page cache (best-effort)"
    sync || true
    echo 3 > /proc/sys/vm/drop_caches || true
  else
    log "No permission to write /proc/sys/vm/drop_caches; continuing"
  fi
}

settle_before() {
  log "Cooldown BEFORE next project: ${SETTLE_SECONDS_BEFORE}s"
  sleep "$SETTLE_SECONDS_BEFORE"
}

settle_after() {
  log "Cooldown AFTER project: ${SETTLE_SECONDS_AFTER}s"
  sleep "$SETTLE_SECONDS_AFTER"
}

copy_if_exists() {
  local src="$1"
  local dst="$2"

  if [ -f "$src" ]; then
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
    log "Copied: $dst"
  fi
}

copy_project_artifacts_if_exist() {
  # run_bench.py has ALWAYS written per-scenario, at results/<scenario>/... --
  # never at the flat results/... this function used to look at (see
  # bench_scenarios.py's _activate_scenario: RESULTS_DIR = ./results/<name>).
  # That meant global_results/ silently collected nothing from any real
  # multi-scenario campaign -- found 2026-09-15 while reviewing this ahead of
  # the real-machine deployment, well before it, not after. Fixed by iterating
  # every results/<label>/ subdirectory a project has (a plain scenario name
  # like "streaming", or a netem-swept one like
  # "tls__netem_rtt_10ms_loss_1pct" from netem/run_rtt_sweep.sh) instead of
  # assuming there is exactly one.
  #
  # Collected under a per-LABEL subdirectory (not flattened with the project
  # name in the filename) specifically so build_master_tables() can point
  # build_master_summary.py at one label's summaries and get a clean,
  # apples-to-apples comparison of the 5 libraries under that one condition --
  # mixing scenarios/netem-points into one global table would silently
  # compare e.g. asio-streaming-loopback against
  # taps-asio-tls-under-10ms-RTT-1pct-loss as if they were peers, which is
  # not a valid comparison.
  local project_dir="$1"
  local project_results_root="$ROOT_DIR/$project_dir/results"

  if [ ! -d "$project_results_root" ]; then
    return
  fi

  local found_any=0
  local label_path
  for label_path in "$project_results_root"/*/; do
    [ -d "$label_path" ] || continue
    local label
    label="$(basename "${label_path%/}")"

    # Never collect the netem sweep's own safety-stash directories (see
    # netem/run_rtt_sweep.sh: stash_preexisting_results / relocate_results) --
    # those are deliberately-preserved leftovers, not this run's results.
    case "$label" in
      *__preexisting_backup_*|*__superseded_*) continue ;;
    esac

    found_any=1
    local results_dir="${label_path%/}"
    local project_out_dir="$PER_PROJECT_DIR/$project_dir/$label"

    mkdir -p \
      "$project_out_dir/raw" \
      "$project_out_dir/summaries" \
      "$project_out_dir/csv" \
      "$project_out_dir/reports" \
      "$project_out_dir/plots" \
      "$RAW_DIR/$label" \
      "$SUMMARIES_DIR/$label" \
      "$CSV_DIR/$label" \
      "$REPORTS_MAIN_WITH_RAW_DIR/$label" \
      "$REPORTS_MAIN_WITHOUT_RAW_DIR/$label" \
      "$REPORTS_COMPARISON_WITH_RAW_DIR/$label" \
      "$REPORTS_COMPARISON_WITHOUT_RAW_DIR/$label" \
      "$REPORTS_PER_LIBRARY_DIR/$label"

    copy_if_exists \
      "$results_dir/raw/macro_bench_results.json" \
      "$project_out_dir/raw/macro_bench_results.json"

    copy_if_exists \
      "$results_dir/raw/macro_bench_summary.json" \
      "$project_out_dir/summaries/macro_bench_summary.json"

    copy_if_exists \
      "$results_dir/raw/macro_bench_results.csv" \
      "$project_out_dir/csv/macro_bench_results.csv"

    copy_if_exists \
      "$results_dir/raw/macro_bench_results.json" \
      "$RAW_DIR/$label/${project_dir}_raw.json"

    # Filename here (not $label) is what build_master_summary.py's
    # infer_library_name() turns into the "library" -- keeping it as the
    # plain project name is what makes it match LIBRARY_ORDER/
    # LIBRARY_DISPLAY_NAMES/LIBRARY_COLORS for proper styling in the report.
    copy_if_exists \
      "$results_dir/raw/macro_bench_summary.json" \
      "$SUMMARIES_DIR/$label/${project_dir}_summary.json"

    copy_if_exists \
      "$results_dir/raw/macro_bench_results.csv" \
      "$CSV_DIR/$label/${project_dir}_raw.csv"

    copy_if_exists \
      "$results_dir/reports/macro_bench_report_with_raw.pdf" \
      "$project_out_dir/reports/macro_bench_report_with_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_report_no_raw.pdf" \
      "$project_out_dir/reports/macro_bench_report_no_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_comparison_report_with_raw.pdf" \
      "$project_out_dir/reports/macro_bench_comparison_report_with_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_comparison_report_no_raw.pdf" \
      "$project_out_dir/reports/macro_bench_comparison_report_no_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_report_with_raw.pdf" \
      "$REPORTS_MAIN_WITH_RAW_DIR/$label/${project_dir}_main_with_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_report_no_raw.pdf" \
      "$REPORTS_MAIN_WITHOUT_RAW_DIR/$label/${project_dir}_main_without_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_comparison_report_with_raw.pdf" \
      "$REPORTS_COMPARISON_WITH_RAW_DIR/$label/${project_dir}_comparison_with_raw.pdf"

    copy_if_exists \
      "$results_dir/reports/macro_bench_comparison_report_no_raw.pdf" \
      "$REPORTS_COMPARISON_WITHOUT_RAW_DIR/$label/${project_dir}_comparison_without_raw.pdf"

    if [ -d "$results_dir/reports" ]; then
      find "$results_dir/reports" -maxdepth 1 -type f -name "*.pdf" | while read -r pdf; do
        local name
        name="$(basename "$pdf")"
        copy_if_exists "$pdf" "$REPORTS_PER_LIBRARY_DIR/$label/${project_dir}_${name}"
      done
    fi

    if [ -d "$results_dir/plots" ]; then
      mkdir -p "$PLOTS_DIR/$label/$project_dir"
      cp -r "$results_dir/plots/." "$PLOTS_DIR/$label/$project_dir/" 2>/dev/null || true
      cp -r "$results_dir/plots/." "$project_out_dir/plots/" 2>/dev/null || true
    fi

    # Gated on the one file build_master_tables() actually needs
    # (macro_bench_summary.json) -- this used to log the same "Copied
    # artifacts" success line unconditionally, even for a label directory
    # that only ever got as far as results/<label>/logs/ before its
    # run_bench.py crashed (e.g. udp_k64 timing out in wait_for_server,
    # 2026-09-18): nothing meaningful was collected, but the log read like
    # a clean success, masking exactly the kind of failure this line exists
    # to surface. copy_if_exists is silent on a missing source by design
    # (most labels legitimately lack some optional artifact), so this is
    # the one line in the whole function that should NOT lie.
    if [ -f "$SUMMARIES_DIR/$label/${project_dir}_summary.json" ]; then
      log "Copied artifacts for $project_dir / $label"
    else
      log "WARNING: no macro_bench_summary.json found for $project_dir / $label -- that scenario/grid-point likely never completed (check its results/$label/logs/)"
    fi
  done

  if [ "$found_any" -eq 0 ]; then
    log "No results/<scenario> subdirectories found for $project_dir -- nothing to collect"
  fi
}

run_project_once() {
  local project_dir="$1"
  local full_dir="$ROOT_DIR/$project_dir"
  local run_script="$full_dir/scripts/run_bench.py"

  if [ ! -d "$full_dir" ]; then
    log "Skipping $project_dir: directory not found"
    return
  fi

  if [ ! -f "$run_script" ]; then
    log "Skipping $project_dir: scripts/run_bench.py not found"
    return
  fi

  trash_caches_best_effort
  settle_before

  log "Running benchmarks for $project_dir"
  local rc=0
  ( cd "$full_dir" && python3 scripts/run_bench.py ) || rc=$?

  # Collect whatever this project DID produce even on failure -- a crash
  # partway through (e.g. scenario 5 of 7) still leaves earlier scenarios'
  # checkpointed results on disk, worth keeping rather than discarding.
  copy_project_artifacts_if_exist "$project_dir"
  settle_after

  if [ "$rc" -ne 0 ]; then
    log "ERROR: run_bench.py failed for $project_dir (exit $rc) -- see the traceback above."
    log "Continuing with the remaining projects instead of aborting the whole campaign (a crash in one project must not silently skip every project queued after it)."
    return 1
  fi
}

warmup_phase() {
  if [ "$WARMUP_ENABLED" != "1" ]; then
    log "Warmup disabled"
    return
  fi

  log "Warmup phase enabled"

  local count=0
  for project_dir in "${PROJECT_DIRS[@]}"; do
    if [ "$count" -ge "$WARMUP_PROJECTS" ]; then
      break
    fi

    local full_dir="$ROOT_DIR/$project_dir"
    local run_script="$full_dir/scripts/run_bench.py"

    if [ -d "$full_dir" ] && [ -f "$run_script" ]; then
      log "Warmup with $project_dir"
      (
        cd "$full_dir"
        python3 scripts/run_bench.py >/dev/null 2>&1 || true
      )
      count=$((count + 1))
      settle_after
    fi
  done
}

build_master_tables() {
  local script="$ROOT_DIR/build_master_summary.py"

  if [ ! -f "$script" ]; then
    log "build_master_summary.py not found; global master table will not be generated"
    return
  fi

  if [ ! -d "$SUMMARIES_DIR" ] || [ -z "$(find "$SUMMARIES_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ]; then
    log "No per-label summaries collected yet; skipping master table generation"
    return
  fi

  # One master table/PDF per label (scenario, or scenario+netem-point) --
  # see copy_project_artifacts_if_exist for why these must not be merged
  # into a single cross-scenario table.
  local label_dir label
  for label_dir in "$SUMMARIES_DIR"/*/; do
    [ -d "$label_dir" ] || continue
    label="$(basename "${label_dir%/}")"
    log "Generating comparison table and PDF report for: $label"
    mkdir -p "$GLOBAL_PLOTS_DIR/$label"

    # capy-corosio's TLS runtime is a known ~20x+ outlier (openssl_stream
    # lacking a compound read/write op -- see design/tls_experiment_notes.md;
    # jmcruz's call: keep its data, keep it out of the plots so it doesn't
    # compress the other 4 arms' axis). --exclude-from-plots only affects the
    # PDF's plots/best-of tables -- the JSON/CSV for this label still include
    # corosio in full, and it gets its own upstream report separately.
    local plot_exclude_args=()
    case "$label" in
      tls|tls__*|tls_framed|tls_framed__*)
        plot_exclude_args=(--exclude-from-plots capy-corosio)
        ;;
    esac

    # Deliberately not fatal (this script has `set -e`): build_master_summary.py
    # exits non-zero when a label has zero *_summary.json files (e.g. every
    # run for that scenario/grid-point failed -- udp_k64 did, for real,
    # 2026-09-18, see is_udp_server_ready in the 5 run_bench.py files). An
    # unguarded call here would abort this whole loop right there under
    # set -e, silently dropping the master table for every alphabetically
    # LATER label too -- confirmed this is exactly why whole_object's master
    # table went missing that run despite its per-project data being
    # complete and sitting right there in $SUMMARIES_DIR/whole_object/: it
    # sorts after udp_k64, so the loop never reached it. One broken scenario
    # must not cost every other, unrelated scenario its comparison table.
    if ! python3 "$script" \
      --input-dir "${label_dir%/}" \
      --json-out "$SUMMARIES_DIR/master_summary__${label}.json" \
      --csv-out "$CSV_DIR/master_summary__${label}.csv" \
      --pdf-out "$GLOBAL_REPORTS_DIR/master__${label}.pdf" \
      --plots-dir "$GLOBAL_PLOTS_DIR/$label" \
      "${plot_exclude_args[@]}"; then
      log "WARNING: build_master_summary.py failed for label '$label' (see the message above -- likely no successful runs for this scenario/grid-point). Continuing with the remaining labels instead of losing their master tables too."
    fi
  done

  log "Master tables written under $SUMMARIES_DIR/master_summary__<label>.json (+ .csv, + PDF under $GLOBAL_REPORTS_DIR)"
}

merge_reports() {
  if [ "$MERGE_PDFS_AT_END" != "1" ]; then
    log "PDF merging disabled"
    return
  fi

  local script="$ROOT_DIR/merge.py"

  if [ ! -f "$script" ]; then
    log "merge.py not found; report merging skipped"
    return
  fi

  log "Merging categorized PDF reports..."
  # Deliberately not fatal (this script has `set -e`): the master tables --
  # the actually valuable output -- are already written to disk by the time
  # this runs. Losing the convenience "one merged PDF" step (e.g. because
  # pypdf isn't importable for some reason) must not abort the whole
  # campaign or look like everything failed. Found 2026-09-15 testing this
  # against real data on WSL2, where pypdf genuinely wasn't installed yet.
  if ! python3 "$script" --input-dir "$REPORTS_DIR" --output-dir "$MERGED_REPORTS_DIR"; then
    log "WARNING: merge.py failed (see the traceback above, likely a missing 'pypdf' module)."
    log "Every per-label master table/CSV/PDF is still valid -- only the merged-PDF" \
        "convenience step was skipped."
  fi
}

main() {
  prepare_dirs
  save_manifest
  save_system_info

  log "Repository root: $ROOT_DIR"
  log "Global results directory: $GLOBAL_RESULTS_DIR"

  if ! need_cmd python3; then
    echo "Error: python3 is not available"
    exit 1
  fi

  measure_idle_if_available

  local projects=("${PROJECT_DIRS[@]}")

  if [ "$RANDOMIZE_ORDER" = "1" ]; then
    if need_cmd shuf; then
      mapfile -t projects < <(printf '%s\n' "${PROJECT_DIRS[@]}" | shuf)
      log "Randomized project order enabled: ${projects[*]}"
    else
      log "shuf not available; keeping the original project order"
    fi
  fi

  warmup_phase

  local failed_projects=()
  for project_dir in "${projects[@]}"; do
    run_project_once "$project_dir" || failed_projects+=("$project_dir")
  done

  build_master_tables
  merge_reports

  log "Global execution finished"
  log "Master tables (one per scenario/netem-point label): $SUMMARIES_DIR/master_summary__<label>.json"
  log "Master CSVs: $CSV_DIR/master_summary__<label>.csv"
  log "Master PDFs: $GLOBAL_REPORTS_DIR/master__<label>.pdf"
  log "Merged reports directory: $MERGED_REPORTS_DIR"

  if [ "${#failed_projects[@]}" -gt 0 ]; then
    log "WARNING: the following project(s) hit an error during their campaign: ${failed_projects[*]}"
    log "Every other project was still attempted and its results (if any) collected above -- this failure is reported, not silent or total."
    exit 1
  fi
}

main "$@"
