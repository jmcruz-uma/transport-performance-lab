#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PROJECT_DIRS=(
  "asio"
  "taps-asio"
  "async-berkeley"
  "bsd-sockets"
  "capy-corosio"
)

log() {
  printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1
}

run_privileged() {
  if [ "${EUID:-$(id -u)}" -eq 0 ]; then
    "$@"
  elif need_cmd sudo; then
    sudo "$@"
  else
    echo "Error: elevated privileges are required to run: $*"
    exit 1
  fi
}

ensure_basic_tools() {
  local missing=0

  for cmd in python3 cmake git; do
    if ! need_cmd "$cmd"; then
      echo "Error: missing required tool: $cmd"
      missing=1
    fi
  done

  if [ "$missing" -ne 0 ]; then
    exit 1
  fi
}

ensure_matplotlib() {
  if python3 -c 'import matplotlib' >/dev/null 2>&1; then
    log "python3-matplotlib is already available"
    return
  fi

  log "Installing python3-matplotlib..."
  if need_cmd apt-get; then
    run_privileged apt-get update
    run_privileged apt-get install -y python3-matplotlib
  else
    log "apt-get is not available; trying pip --user"
    if ! python3 -m pip install --user matplotlib; then
      echo "Error: failed to install matplotlib automatically"
      exit 1
    fi
  fi
}

ensure_pip() {
  if python3 -m pip --version >/dev/null 2>&1; then
    return
  fi

  log "pip is not available; trying to install python3-pip..."
  if need_cmd apt-get; then
    run_privileged apt-get update
    run_privileged apt-get install -y python3-pip python3-venv
  else
    echo "Error: python3-pip is not available and cannot be installed automatically"
    exit 1
  fi
}

ensure_pypdf() {
  if python3 -c 'import pypdf' >/dev/null 2>&1; then
    log "Python module 'pypdf' is already available"
    return
  fi

  log "Installing Python module 'pypdf'..."

  if need_cmd apt-get; then
    if run_privileged apt-get install -y python3-pypdf 2>/dev/null; then
      log "pypdf installed with apt"
      return
    fi
  fi

  ensure_pip

  if python3 -m pip install --user pypdf; then
    log "pypdf installed with pip --user"
    return
  fi

  echo "Error: failed to install pypdf"
  exit 1
}

ensure_iproute2() {
  if need_cmd tc; then
    log "iproute2 (tc) is already available"
  else
    log "Installing iproute2 (provides tc, needed by the netem D7 RTT sweep)..."
    if need_cmd apt-get; then
      run_privileged apt-get update
      run_privileged apt-get install -y iproute2
    else
      echo "Error: 'tc' is not available and apt-get is not present to install it"
      exit 1
    fi
  fi

  # sch_netem is often a loadable module (and sometimes built into the kernel,
  # in which case modprobe correctly no-ops with "module already builtin").
  # Best-effort: the netem sweep script re-checks this itself before shaping,
  # so a failure here does not abort the whole build.
  run_privileged modprobe sch_netem 2>/dev/null || true
}

ensure_netem_nopasswd_sudo() {
  if [ "${EUID:-$(id -u)}" -eq 0 ]; then
    log "Running as root: no separate sudo rule needed for tc/ip"
    return
  fi

  if ! need_cmd tc || ! need_cmd ip; then
    log "tc/ip not available yet; skipping the netem sudo rule"
    return
  fi

  local tc_path ip_path sudoers_file target_user
  tc_path="$(command -v tc)"
  ip_path="$(command -v ip)"
  sudoers_file="/etc/sudoers.d/tc-netem"
  target_user="${SUDO_USER:-$(id -un)}"

  if sudo -n "$tc_path" qdisc show dev lo >/dev/null 2>&1 && \
     sudo -n "$ip_path" netns list >/dev/null 2>&1; then
    log "Passwordless sudo for tc and ip is already working"
    return
  fi

  log "Installing a NOPASSWD sudo rule scoped to '$tc_path' and '$ip_path' for user '$target_user'" \
      "(so the netem D7 netns+veth RTT/loss sweep can manage the topology and qdiscs without prompting)"

  local rule="$target_user ALL=(ALL) NOPASSWD: $tc_path, $ip_path"
  if ! echo "$rule" | run_privileged tee "$sudoers_file" >/dev/null; then
    echo "Error: failed to write $sudoers_file"
    exit 1
  fi
  run_privileged chmod 0440 "$sudoers_file"

  if ! run_privileged visudo -c >/dev/null 2>&1; then
    echo "Error: $sudoers_file failed sudoers syntax validation; removing it"
    run_privileged rm -f "$sudoers_file"
    exit 1
  fi

  log "tc/ip sudo rule installed and validated"
}

setup_tls_assets() {
  log "Preparing shared TLS assets (certificates + deterministic payload + framed manifest)"
  chmod +x "$ROOT_DIR/tls/gen_certs.sh" "$ROOT_DIR/tls/gen_payload.sh"
  "$ROOT_DIR/tls/gen_certs.sh"
  "$ROOT_DIR/tls/gen_payload.sh"
  python3 "$ROOT_DIR/tls/gen_manifest.py"
}

build_project() {
  local project_dir="$1"
  local full_dir="$ROOT_DIR/$project_dir"
  local build_script="$full_dir/build_release.sh"

  if [ ! -d "$full_dir" ]; then
    log "Skipping $project_dir: directory not found"
    return
  fi

  if [ ! -f "$build_script" ]; then
    log "Skipping $project_dir: build_release.sh not found"
    return
  fi

  log "Building $project_dir"
  chmod +x "$build_script"

  (
    cd "$full_dir"
    ./build_release.sh
  )
}

main() {
  log "Repository root: $ROOT_DIR"

  ensure_basic_tools
  ensure_matplotlib
  ensure_pypdf
  ensure_iproute2
  ensure_netem_nopasswd_sudo
  setup_tls_assets

  for project_dir in "${PROJECT_DIRS[@]}"; do
    build_project "$project_dir"
  done

  log "Global build completed"
}

main "$@"