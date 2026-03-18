#!/usr/bin/env bash
set -euo pipefail

# === Configuration ===
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BRANCH_ID="$(basename "$PROJECT_ROOT")"

# Windows: LOCALAPPDATA, Linux/Mac: XDG_DATA_HOME or ~/.local/share
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    DEFAULT_QUEUE_DIR="${LOCALAPPDATA}/apex-build-queue"
else
    DEFAULT_QUEUE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-build-queue"
fi

QUEUE_DIR="${APEX_BUILD_QUEUE_DIR:-$DEFAULT_QUEUE_DIR}"
STALE_TIMEOUT="${APEX_STALE_TIMEOUT_SEC:-3600}"
POLL_INTERVAL="${APEX_POLL_INTERVAL_SEC:-1}"
MY_QUEUE_FILE=""

# === Init ===
do_init() {
    mkdir -p "$QUEUE_DIR/build-queue"
    mkdir -p "$QUEUE_DIR/merge-queue"
    mkdir -p "$QUEUE_DIR/logs"
}

# === Main ===
main() {
    local channel="${1:-help}"
    shift || true

    case "$channel" in
        init)   do_init ;;
        build)  do_init; echo "build channel: not yet implemented" ;;
        merge)  do_init; echo "merge channel: not yet implemented" ;;
        *)      echo "Usage: queue-lock.sh <init|build|merge> [args...]"; exit 1 ;;
    esac
}

main "$@"
