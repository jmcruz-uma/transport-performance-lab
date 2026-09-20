#!/usr/bin/env bash
# Standalone global-results collection + master-table generation, factored
# out of run.sh so it can be run again AFTER the D7 netem sweep too.
#
# Why this needs to exist separately from run.sh: run.sh already collects
# and builds master tables for whatever labels exist at the point it
# finishes (the loopback baseline, RTT=0) -- but run_everything.sh's stage 5
# (netem/run_rtt_sweep.sh) runs AFTER run.sh and creates dozens/hundreds more
# labels (results/<scenario>__netem_rtt_<R>ms_loss_<L>pct/ in every project).
# Nothing was re-running the collection afterwards, so the entire D7 sweep --
# the main new experiment this repo was extended for -- would sit correctly
# in each project's results/ but NEVER make it into global_results/ or get a
# master comparison table. Found 2026-09-15, auditing the deployment plan for
# gaps ahead of the real-machine run, specifically because of that question.
# run_everything.sh now calls this script as its final stage, after both
# run.sh and the netem sweep have finished, so every label -- baseline and
# every D7 grid point -- gets collected in one pass. Safe/idempotent to also
# run by hand at any time (e.g. mid-campaign, to check progress): re-copying
# already-collected files is harmless, and copy_if_exists silently skips a
# scenario that hasn't produced results yet.
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

MERGE_PDFS_AT_END="${MERGE_PDFS_AT_END:-1}"

PROJECT_DIRS=(
  "asio"
  "taps-asio"
  "async-berkeley"
  "bsd-sockets"
  "capy-corosio"
)

log() {
  printf '[%s] [collect-results] %s\n' "$(date '+%H:%M:%S')" "$*"
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

# Identical to run.sh's copy_project_artifacts_if_exist -- see its comments
# there for the full rationale (the flat-path bug this replaced, and why
# results are kept per-label rather than merged).
copy_project_artifacts_if_exist() {
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

# Identical to run.sh's build_master_tables -- one master table/PDF per
# label, never a single table mixing scenarios/netem-points together.
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

  local label_dir label
  for label_dir in "$SUMMARIES_DIR"/*/; do
    [ -d "$label_dir" ] || continue
    label="$(basename "${label_dir%/}")"
    log "Generating comparison table and PDF report for: $label"
    mkdir -p "$GLOBAL_PLOTS_DIR/$label"

    # capy-corosio's TLS runtime looked like a ~20x+ outlier in an earlier,
    # WSL2-loopback pilot (openssl_stream lacking a compound read/write op --
    # see design/tls_experiment_notes.md) and was excluded from tls/tls_framed
    # plots on that basis. That did not reproduce on real hardware (2026-09-18
    # real-machine data: corosio is competitive with, sometimes faster than,
    # the rest of the cluster under TLS) -- the exclusion is gone, corosio's
    # numbers are plotted like everyone else's again. --exclude-from-plots
    # itself stays available on build_master_summary.py as a general-purpose
    # option, just unused here now.
    local plot_exclude_args=()

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
  # the actually valuable output of this whole script -- are already written
  # to disk by the time this runs. Losing the convenience "one merged PDF"
  # step (e.g. because pypdf isn't importable for some reason) must not look
  # like the whole collection failed and must not discard everything that
  # already succeeded. Found 2026-09-15 testing this against real data on
  # WSL2, where pypdf genuinely wasn't installed yet.
  if ! python3 "$script" --input-dir "$REPORTS_DIR" --output-dir "$MERGED_REPORTS_DIR"; then
    log "WARNING: merge.py failed (see the traceback above, likely a missing 'pypdf' module)."
    log "Every per-label master table/CSV/PDF above is still valid -- only the merged-PDF" \
        "convenience step was skipped. Fix pypdf (preflight.sh does) and rerun this script" \
        "if you want the merged PDFs too; nothing else needs to be redone."
  fi
}

# D2 comparability check (tls/check_identity.py): confirms every arm's TLS
# server+client actually negotiated identical parameters, across the
# baseline AND every D7 netem grid point -- catches e.g. one arm rebuilt
# after a system OpenSSL upgrade while the others weren't. Deliberately not
# fatal (this script has `set -e` and this must not discard the collection
# work already done above): a mismatch means "don't trust cross-arm TLS
# comparisons yet", not "this run produced nothing useful" -- every
# non-TLS scenario's results are unaffected either way. Was a manual,
# easy-to-forget step before 2026-09-15; now runs every time collection
# does, so it can't be forgotten across 170+ unattended grid points.
run_identity_check() {
  local script="$ROOT_DIR/tls/check_identity.py"
  local out="$GLOBAL_RESULTS_DIR/tls_identity_check.txt"

  if [ ! -f "$script" ]; then
    log "tls/check_identity.py not found; skipping the D2 comparability check"
    return
  fi

  log "Running the D2 TLS comparability check (tls/check_identity.py)..."
  if python3 "$script" --root "$ROOT_DIR" > "$out" 2>&1; then
    log "D2 check: OK -- every arm's TLS parameters agree (full output: $out)"
  else
    log "*********************************************************************"
    log "WARNING: D2 TLS comparability check FAILED -- see $out"
    log "This means at least one arm's negotiated TLS parameters (OpenSSL"
    log "version/cipher/ALPN) drifted from the others, for the tls/tls_framed"
    log "scenarios (baseline and/or a netem grid point). Every OTHER scenario's"
    log "results are unaffected. Do not treat cross-arm TLS comparisons as"
    log "valid until this is investigated -- read $out for exactly which"
    log "arm/label disagreed."
    log "*********************************************************************"
  fi
}

main() {
  log "Repository root: $ROOT_DIR"
  log "Global results directory: $GLOBAL_RESULTS_DIR"

  if ! need_cmd python3; then
    echo "Error: python3 is not available"
    exit 1
  fi

  prepare_dirs

  for project_dir in "${PROJECT_DIRS[@]}"; do
    copy_project_artifacts_if_exist "$project_dir"
  done

  build_master_tables
  merge_reports
  run_identity_check

  log "Collection finished."
  log "Master tables (one per scenario/netem-point label): $SUMMARIES_DIR/master_summary__<label>.json"
  log "Master CSVs: $CSV_DIR/master_summary__<label>.csv"
  log "Master PDFs: $GLOBAL_REPORTS_DIR/master__<label>.pdf"
  log "Merged reports directory: $MERGED_REPORTS_DIR"
  log "D2 TLS comparability check: $GLOBAL_RESULTS_DIR/tls_identity_check.txt"
}

main "$@"
