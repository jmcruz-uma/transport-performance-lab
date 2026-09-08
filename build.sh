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
  setup_tls_assets

  for project_dir in "${PROJECT_DIRS[@]}"; do
    build_project "$project_dir"
  done

  log "Global build completed"
}

main "$@"