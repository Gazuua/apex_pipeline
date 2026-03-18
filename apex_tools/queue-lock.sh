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

# === Lock Acquisition ===
try_lock() {
    local channel="$1"
    local lock_dir="$QUEUE_DIR/${channel}.lock"

    if mkdir "$lock_dir" 2>/dev/null; then
        local owner_file="$QUEUE_DIR/${channel}.owner"
        {
            echo "PID=$$"
            echo "BRANCH=$BRANCH_ID"
            echo "ACQUIRED=$(date +%s)"
            echo "STATUS=ACTIVE"
        } > "$owner_file"
        return 0
    fi
    return 1
}

release_lock() {
    local channel="$1"
    rmdir "$QUEUE_DIR/${channel}.lock" 2>/dev/null || rm -rf "$QUEUE_DIR/${channel}.lock"
    rm -f "$QUEUE_DIR/${channel}.owner"
}

acquire_lock() {
    local channel="$1"
    register_queue "$channel"

    local printed_wait=false

    while true; do
        cleanup_orphan_queue "$channel"
        detect_stale_lock "$channel" || true

        if get_queue_position "$channel" && try_lock "$channel"; then
            rm -f "$MY_QUEUE_FILE"
            echo "[queue-lock] lock acquired: $channel (branch=$BRANCH_ID, pid=$$)"
            return 0
        fi

        if [[ "$printed_wait" == false ]]; then
            local depth owner_info=""
            depth=$(get_queue_depth "$channel")
            if [[ -f "$QUEUE_DIR/${channel}.owner" ]]; then
                local owner_branch owner_pid
                owner_branch=$(grep '^BRANCH=' "$QUEUE_DIR/${channel}.owner" | cut -d= -f2)
                owner_pid=$(grep '^PID=' "$QUEUE_DIR/${channel}.owner" | cut -d= -f2)
                owner_info=" (current: $owner_branch, PID $owner_pid)"
            fi
            echo "[queue-lock] waiting for $channel lock... queue depth: $depth$owner_info"
            printed_wait=true
        fi
        sleep "$POLL_INTERVAL"
    done
}

# === Channel: build ===
do_build() {
    local preset="${1:-debug}"
    shift || true
    local extra_args=("$@")

    echo "[queue-lock] build requested: preset=$preset, args=${extra_args[*]:-none}, branch=$BRANCH_ID"

    acquire_lock "build"
    trap 'release_lock "build"' EXIT

    local log_file="$QUEUE_DIR/logs/${BRANCH_ID}.log"
    local -a build_cmd=(cmd.exe //c "${PROJECT_ROOT}\\build.bat" "$preset" "${extra_args[@]}")

    echo "[queue-lock] starting build: ${build_cmd[*]}"
    echo "[queue-lock] log: $log_file"

    local exit_code=0
    "${build_cmd[@]}" 2>&1 | tee "$log_file" || exit_code=$?

    if [[ $exit_code -eq 0 ]]; then
        echo "[queue-lock] build completed successfully"
    else
        echo "[queue-lock] build FAILED (exit code: $exit_code)"
    fi

    exit $exit_code
}

# === Channel: merge ===
do_merge() {
    local subcmd="${1:-help}"
    shift || true

    case "$subcmd" in
        acquire)  do_merge_acquire ;;
        release)  do_merge_release ;;
        status)   do_merge_status ;;
        conflict) do_merge_conflict ;;
        *)        echo "Usage: queue-lock.sh merge <acquire|release|status|conflict>"; exit 1 ;;
    esac
}

do_merge_acquire() {
    echo "[queue-lock] merge lock requested: branch=$BRANCH_ID"
    acquire_lock "merge"

    sed -i 's/^STATUS=.*/STATUS=MERGING/' "$QUEUE_DIR/merge.owner" 2>/dev/null || true

    echo "[queue-lock] merge lock acquired. Proceed with merge workflow."
    echo "[queue-lock] IMPORTANT: run 'queue-lock.sh merge release' when done."

    # acquire/release 패턴: 비정상 종료 시 안전망으로 자동 해제
    trap 'echo "[queue-lock] WARNING: merge lock auto-released on exit"; release_lock "merge"' EXIT
}

do_merge_release() {
    echo "[queue-lock] releasing merge lock"
    release_lock "merge"
    echo "[queue-lock] merge lock released"
}

do_merge_conflict() {
    local owner_file="$QUEUE_DIR/merge.owner"
    if [[ -f "$owner_file" ]]; then
        sed -i 's/^STATUS=.*/STATUS=CONFLICT/' "$owner_file"
        echo "[queue-lock] merge status set to CONFLICT"
    else
        echo "[queue-lock] ERROR: no merge lock owner file" >&2
        exit 1
    fi
}

do_merge_status() {
    local lock_dir="$QUEUE_DIR/merge.lock"
    local owner_file="$QUEUE_DIR/merge.owner"
    local queue_dir="$QUEUE_DIR/merge-queue"

    if [[ -d "$lock_dir" && -f "$owner_file" ]]; then
        echo "LOCK=HELD"
        grep '^BRANCH=' "$owner_file" | sed 's/BRANCH=/OWNER=/'
        grep '^PID=' "$owner_file"
        grep '^ACQUIRED=' "$owner_file"
        grep '^STATUS=' "$owner_file"
    else
        echo "LOCK=FREE"
        echo "STATUS=IDLE"
    fi

    local depth
    depth=$(ls "$queue_dir" 2>/dev/null | wc -l | tr -d ' ')
    echo "QUEUE_DEPTH=$depth"

    if [[ "$depth" -gt 0 ]]; then
        local entries
        entries=$(ls "$queue_dir" 2>/dev/null | sort | tr '\n' ',')
        echo "QUEUE=${entries%,}"
    fi
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
        build)  do_init; do_build "$@" ;;
        merge)  do_init; do_merge "$@" ;;
        _check_pid)    check_pid_alive "$1" ;;
        _detect_stale) detect_stale_lock "$1" ;;
        _cleanup_queue) cleanup_orphan_queue "$1" ;;
        _try_lock)     try_lock "$1" ;;
        _acquire_lock) acquire_lock "$1" ;;
        _release_lock) release_lock "$1" ;;
        *)      echo "Usage: queue-lock.sh <init|build|merge> [args...]"; exit 1 ;;
    esac
}

main "$@"
