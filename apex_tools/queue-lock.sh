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

# === PID Check ===
check_pid_alive() {
    local pid="$1"
    # kill -0: bash/MSYS PID 확인 (우리가 저장하는 $$는 항상 bash PID)
    if kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    # fallback: Windows 네이티브 프로세스 확인 (cmd.exe 등)
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        tasklist //FI "PID eq $pid" 2>/dev/null | grep -qi "$pid"
        return $?
    fi
    return 1
}

# === Stale Detection ===
detect_stale_lock() {
    local channel="$1"
    local lock_dir="$QUEUE_DIR/${channel}.lock"
    local owner_file="$QUEUE_DIR/${channel}.owner"

    [[ ! -d "$lock_dir" ]] && return 1

    if [[ ! -f "$owner_file" ]]; then
        echo "[queue-lock] WARNING: orphan lock detected (no owner), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        return 0
    fi

    local pid acquired
    pid=$(grep '^PID=' "$owner_file" | cut -d= -f2)
    acquired=$(grep '^ACQUIRED=' "$owner_file" | cut -d= -f2)

    if ! check_pid_alive "$pid"; then
        echo "[queue-lock] WARNING: stale lock (PID $pid dead), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        rm -f "$owner_file"
        return 0
    fi

    local now elapsed
    now=$(date +%s)
    elapsed=$((now - acquired))
    if [[ $elapsed -ge $STALE_TIMEOUT ]]; then
        echo "[queue-lock] WARNING: stale lock (${elapsed}s elapsed > ${STALE_TIMEOUT}s timeout), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        rm -f "$owner_file"
        return 0
    fi

    return 1
}

# === Queue Management ===
register_queue() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S)
    MY_QUEUE_FILE="$queue_dir/${timestamp}_${BRANCH_ID}_$$"
    touch "$MY_QUEUE_FILE"
}

get_queue_position() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    local first
    first=$(ls "$queue_dir" 2>/dev/null | sort | head -1)

    [[ -z "$first" ]] && return 1

    if [[ "$(basename "$MY_QUEUE_FILE")" == "$first" ]]; then
        return 0
    else
        return 1
    fi
}

get_queue_depth() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    ls "$queue_dir" 2>/dev/null | wc -l | tr -d ' '
}

cleanup_orphan_queue() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"

    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    for entry in "$queue_dir"/*; do
        [[ ! -f "$entry" ]] && continue
        local filename pid
        filename=$(basename "$entry")
        pid=${filename##*_}
        if ! check_pid_alive "$pid"; then
            echo "[queue-lock] orphan queue entry removed: $filename (PID $pid dead)"
            rm -f "$entry"
        fi
    done

    eval "$prev_nullglob" 2>/dev/null || true
}

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
        _check_pid)    check_pid_alive "$1" ;;
        _detect_stale) detect_stale_lock "$1" ;;
        _cleanup_queue) cleanup_orphan_queue "$1" ;;
        *)      echo "Usage: queue-lock.sh <init|build|merge> [args...]"; exit 1 ;;
    esac
}

main "$@"
