#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

set -euo pipefail

# ============================================================================
# branch-handoff.sh — 브랜치 간 중앙 인수인계 시스템
#
# 물리적으로 격리된 워크스페이스의 병렬 에이전트들이 파일 기반으로 소통하여
# 충돌·중복 작업·재작업을 사전 방지한다.
#
# 사용법: branch-handoff.sh <command> [options]
# 명령어: init, notify, check, ack, read, status, backlog-check, cleanup
# ============================================================================

# === Configuration ===
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BRANCH_ID="${BRANCH_ID:-$(basename "$PROJECT_ROOT" | sed 's/apex_pipeline_//')}"

if [[ -n "${LOCALAPPDATA:-}" ]]; then
    DEFAULT_HANDOFF_DIR="${LOCALAPPDATA}/apex-branch-handoff"
else
    DEFAULT_HANDOFF_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
fi

HANDOFF_DIR="${APEX_HANDOFF_DIR:-$DEFAULT_HANDOFF_DIR}"
INDEX_LOCK_TIMEOUT=30
STALE_TIMEOUT="${APEX_HANDOFF_STALE_SEC:-86400}"

# === PID Check (queue-lock.sh와 동일) ===
check_pid_alive() {
    local pid="$1"
    if kill -0 "$pid" 2>/dev/null; then return 0; fi
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
        tasklist //FI "PID eq $pid" 2>/dev/null | grep -qi "$pid"
        return $?
    fi
    return 1
}

# === Init ===
do_init() {
    mkdir -p "$HANDOFF_DIR/watermarks"
    mkdir -p "$HANDOFF_DIR/payloads"
    mkdir -p "$HANDOFF_DIR/active"
    mkdir -p "$HANDOFF_DIR/responses"
    mkdir -p "$HANDOFF_DIR/backlog-status"
    mkdir -p "$HANDOFF_DIR/archive"
    [[ -f "$HANDOFF_DIR/index" ]] || touch "$HANDOFF_DIR/index"
}

# === Index Locking (mkdir atomic lock) ===
lock_index() {
    while ! mkdir "$HANDOFF_DIR/index.lock" 2>/dev/null; do
        local lock_pid_file="$HANDOFF_DIR/index.lock/owner"
        if [[ -f "$lock_pid_file" ]]; then
            local lock_pid lock_time
            lock_pid=$(grep '^PID=' "$lock_pid_file" | cut -d= -f2)
            lock_time=$(grep '^ACQUIRED=' "$lock_pid_file" | cut -d= -f2)
            if ! check_pid_alive "$lock_pid"; then
                echo "[handoff] stale index lock (PID $lock_pid dead), breaking" >&2
                rm -rf "$HANDOFF_DIR/index.lock"
                continue
            fi
            local now elapsed
            now=$(date +%s)
            elapsed=$((now - lock_time))
            if [[ $elapsed -ge $INDEX_LOCK_TIMEOUT ]]; then
                echo "[handoff] stale index lock (${elapsed}s), breaking" >&2
                rm -rf "$HANDOFF_DIR/index.lock"
                continue
            fi
        else
            # lock dir exists but no owner file — orphan
            rm -rf "$HANDOFF_DIR/index.lock"
            continue
        fi
        sleep 0.1
    done
    {
        echo "PID=$$"
        echo "ACQUIRED=$(date +%s)"
    } > "$HANDOFF_DIR/index.lock/owner"
}

unlock_index() {
    rm -rf "$HANDOFF_DIR/index.lock" 2>/dev/null
}

append_index() {
    local tier="$1" branch="$2" scopes="$3" summary="$4"
    lock_index
    local last_id
    last_id=$(tail -1 "$HANDOFF_DIR/index" 2>/dev/null | cut -d'|' -f1)
    last_id=${last_id:-0}
    # last_id가 숫자가 아니면 0으로
    if ! [[ "$last_id" =~ ^[0-9]+$ ]]; then
        last_id=0
    fi
    local new_id=$((last_id + 1))
    echo "${new_id}|${tier}|${branch}|${scopes}|${summary}" >> "$HANDOFF_DIR/index"
    unlock_index
    echo "$new_id"
}

# === Argument Parsing ===
ARGS_BACKLOG=""
ARGS_SCOPES=""
ARGS_SUMMARY=""
ARGS_PAYLOAD_FILE=""
ARGS_ID=""
ARGS_ACTION=""
ARGS_REASON=""
ARGS_GATE=""

parse_args() {
    ARGS_BACKLOG=""
    ARGS_SCOPES=""
    ARGS_SUMMARY=""
    ARGS_PAYLOAD_FILE=""
    ARGS_ID=""
    ARGS_ACTION=""
    ARGS_REASON=""
    ARGS_GATE=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --backlog)       ARGS_BACKLOG="$2"; shift 2 ;;
            --scopes)        ARGS_SCOPES="$2"; shift 2 ;;
            --summary)       ARGS_SUMMARY="$2"; shift 2 ;;
            --payload-file)  ARGS_PAYLOAD_FILE="$2"; shift 2 ;;
            --id)            ARGS_ID="$2"; shift 2 ;;
            --action)        ARGS_ACTION="$2"; shift 2 ;;
            --reason)        ARGS_REASON="$2"; shift 2 ;;
            --gate)          ARGS_GATE="$2"; shift 2 ;;
            *)               shift ;;
        esac
    done
}

# === Notify ===
do_notify() {
    local subcmd="${1:-help}"
    shift || true
    parse_args "$@"

    case "$subcmd" in
        start)   do_notify_start ;;
        design)  do_notify_design ;;
        merge)   do_notify_merge ;;
        *)       echo "Usage: branch-handoff.sh notify <start|design|merge> [options]"; exit 1 ;;
    esac
}

do_notify_start() {
    local now
    now=$(date +"%Y-%m-%d %H:%M:%S")

    # active 파일 생성
    {
        echo "branch: $BRANCH_ID"
        echo "pid: $$"
        if [[ -n "$ARGS_BACKLOG" ]]; then echo "backlog: $ARGS_BACKLOG"; fi
        echo "status: started"
        echo "scopes:"
        IFS=',' read -ra scope_arr <<< "$ARGS_SCOPES"
        for s in "${scope_arr[@]}"; do echo "  - $s"; done
        echo "summary: \"$ARGS_SUMMARY\""
        echo "started_at: \"$now\""
        echo "updated_at: \"$now\""
    } > "$HANDOFF_DIR/active/${BRANCH_ID}.yml"

    # backlog-status 파일 생성
    if [[ -n "$ARGS_BACKLOG" ]]; then
        {
            echo "backlog: $ARGS_BACKLOG"
            echo "branch: $BRANCH_ID"
            echo "started_at: \"$now\""
        } > "$HANDOFF_DIR/backlog-status/${BRANCH_ID}.yml"
    fi

    # index에 추가
    local summary_text="$ARGS_SUMMARY"
    if [[ -n "$ARGS_BACKLOG" ]]; then summary_text="BACKLOG-${ARGS_BACKLOG} $summary_text"; fi

    local new_id
    new_id=$(append_index "tier1" "$BRANCH_ID" "$ARGS_SCOPES" "$summary_text")

    echo "[handoff] Tier 1 notification published: #${new_id} (branch=$BRANCH_ID, scopes=$ARGS_SCOPES)"
}

do_notify_design() {
    local now
    now=$(date +"%Y-%m-%d %H:%M:%S")

    # 이전 Tier 2 ID 찾기 (SUPERSEDE용)
    local prev_tier2_id=""
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        prev_tier2_id=$(grep '^latest_tier2_id:' "$HANDOFF_DIR/active/${BRANCH_ID}.yml" 2>/dev/null | awk '{print $2}' || echo "")
    fi

    # summary에 SUPERSEDE 태그 추가
    local summary_text="$ARGS_SUMMARY"
    if [[ -n "$prev_tier2_id" ]]; then summary_text="[SUPERSEDE:${prev_tier2_id}] $summary_text"; fi

    # index에 추가
    local new_id
    new_id=$(append_index "tier2" "$BRANCH_ID" "$ARGS_SCOPES" "$summary_text")

    # payload 파일 생성
    {
        echo "---"
        echo "id: $new_id"
        echo "tier: tier2"
        echo "branch: $BRANCH_ID"
        echo "scopes: [$(echo "$ARGS_SCOPES" | sed 's/,/, /g')]"
        if [[ -n "$prev_tier2_id" ]]; then echo "supersedes: $prev_tier2_id"; fi
        echo "timestamp: \"$now\""
        echo "---"
        echo ""
        if [[ -n "$ARGS_PAYLOAD_FILE" && -f "$ARGS_PAYLOAD_FILE" ]]; then
            cat "$ARGS_PAYLOAD_FILE"
        fi
    } > "$HANDOFF_DIR/payloads/${new_id}.md"

    # active 파일 갱신 (있으면)
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        sed -i "s/^status: .*/status: designing/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        if grep -q '^latest_tier2_id:' "$HANDOFF_DIR/active/${BRANCH_ID}.yml"; then
            sed -i "s/^latest_tier2_id: .*/latest_tier2_id: $new_id/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        else
            echo "latest_tier2_id: $new_id" >> "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        fi
        sed -i "s/^updated_at: .*/updated_at: \"$now\"/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"

        # scopes 블록 교체
        local tmp_file
        tmp_file=$(mktemp)
        local in_scopes=false
        while IFS= read -r line; do
            if [[ "$line" == "scopes:" ]]; then
                in_scopes=true
                echo "scopes:"
                IFS=',' read -ra scope_arr <<< "$ARGS_SCOPES"
                for s in "${scope_arr[@]}"; do echo "  - $s"; done
                continue
            fi
            if [[ "$in_scopes" == true ]]; then
                if [[ "$line" == "  - "* ]]; then
                    continue
                else
                    in_scopes=false
                fi
            fi
            echo "$line"
        done < "$HANDOFF_DIR/active/${BRANCH_ID}.yml" > "$tmp_file"
        mv "$tmp_file" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
    fi

    echo "[handoff] Tier 2 notification published: #${new_id} (branch=$BRANCH_ID, scopes=$ARGS_SCOPES)"
    if [[ -n "$prev_tier2_id" ]]; then echo "[handoff] supersedes: #${prev_tier2_id}"; fi
}

do_notify_merge() {
    # scopes는 active 파일에서 추출
    local scopes=""
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        scopes=$(grep '^  - ' "$HANDOFF_DIR/active/${BRANCH_ID}.yml" | sed 's/  - //' | paste -sd',' -)
    fi
    if [[ -z "$scopes" ]]; then scopes="unknown"; fi

    # 머지 커밋 해시 추출 시도
    local commit_hash=""
    commit_hash=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo "")
    local summary_text="$ARGS_SUMMARY"
    if [[ -n "$commit_hash" ]]; then summary_text="$summary_text [$commit_hash]"; fi

    # index에 추가
    local new_id
    new_id=$(append_index "tier3" "$BRANCH_ID" "$scopes" "$summary_text")

    # active 파일 삭제
    rm -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml"

    # backlog-status 파일 삭제
    rm -f "$HANDOFF_DIR/backlog-status/${BRANCH_ID}.yml"

    echo "[handoff] Tier 3 notification published: #${new_id} (branch=$BRANCH_ID, merge complete)"
}

# === Check ===
do_check() {
    parse_args "$@"

    # SUPERSEDE 맵 구축: supersede된 ID 목록
    local -A superseded_ids
    while IFS='|' read -r id tier branch scopes summary; do
        if [[ -z "$id" ]]; then continue; fi
        local sup_id
        sup_id=$(echo "$summary" | sed -n 's/.*\[SUPERSEDE:\([0-9]*\)\].*/\1/p')
        if [[ -n "$sup_id" ]]; then superseded_ids[$sup_id]=1; fi
    done < "$HANDOFF_DIR/index"

    # 새 알림 수집 (response 파일 기반으로 ack 판별)
    local found=0
    local output_lines=""

    while IFS='|' read -r id tier branch scopes summary; do
        if [[ -z "$id" ]]; then continue; fi
        if [[ "$branch" == "$BRANCH_ID" ]]; then continue; fi  # 자기 알림은 스킵
        if [[ -n "${superseded_ids[$id]:-}" ]]; then continue; fi  # supersede된 건 스킵

        # 이미 ack한 건 스킵
        if [[ -f "$HANDOFF_DIR/responses/${id}/${BRANCH_ID}.yml" ]]; then
            continue
        fi

        # 스코프 필터링
        if [[ -n "${ARGS_SCOPES:-}" ]]; then
            local match=false
            IFS=',' read -ra filter_scopes <<< "$ARGS_SCOPES"
            IFS=',' read -ra notif_scopes <<< "$scopes"
            for fs in "${filter_scopes[@]}"; do
                for ns in "${notif_scopes[@]}"; do
                    if [[ "$fs" == "$ns" ]]; then match=true; fi
                done
            done
            if [[ "$match" == false ]]; then continue; fi
        fi

        found=$((found + 1))
        local payload_info=""
        if [[ -f "$HANDOFF_DIR/payloads/${id}.md" ]]; then
            payload_info="payload: payloads/${id}.md"
        elif [[ "$tier" == "tier3" ]]; then
            payload_info="(no payload — check remote commits)"
        fi

        output_lines+="  #${id} ${tier} ${branch} ${scopes}"$'\n'
        output_lines+="     \"${summary}\""$'\n'
        if [[ -n "$payload_info" ]]; then output_lines+="     ${payload_info}"$'\n'; fi
        output_lines+=""$'\n'
    done < "$HANDOFF_DIR/index"

    if [[ "$found" -eq 0 ]]; then
        if [[ -n "${ARGS_GATE:-}" ]]; then
            echo "[GATE:${ARGS_GATE}] CLEAR — no pending notifications."
        else
            echo "No new notifications."
        fi
        return 0
    fi

    if [[ -n "${ARGS_GATE:-}" ]]; then
        local scope_label="${ARGS_SCOPES:-all}"
        echo "[GATE:${ARGS_GATE}] BLOCKED — ${found} unacked notification(s) matching scopes [${scope_label}]"
        echo ""
        echo -n "$output_lines"
        echo "Resolve all before proceeding."
        return 1
    else
        echo "${found} new notification(s):"
        echo ""
        echo -n "$output_lines"
        return 0
    fi
}

# === Ack ===
do_ack() {
    parse_args "$@"

    if [[ -z "$ARGS_ID" || -z "$ARGS_ACTION" ]]; then
        echo "Usage: branch-handoff.sh ack --id <ID> --action <ACTION> --reason <REASON>"
        exit 1
    fi

    # action 유효성 검사
    case "$ARGS_ACTION" in
        no-impact|will-rebase|rebased|design-adjusted|deferred) ;;
        *) echo "[handoff] ERROR: invalid action '$ARGS_ACTION'"; exit 1 ;;
    esac

    local now
    now=$(date +"%Y-%m-%d %H:%M:%S")

    # response 파일 생성
    mkdir -p "$HANDOFF_DIR/responses/${ARGS_ID}"
    {
        echo "notification_id: $ARGS_ID"
        echo "branch: $BRANCH_ID"
        echo "action: $ARGS_ACTION"
        echo "reason: \"${ARGS_REASON:-}\""
        echo "timestamp: \"$now\""
    } > "$HANDOFF_DIR/responses/${ARGS_ID}/${BRANCH_ID}.yml"

    echo "[handoff] ack: #${ARGS_ID} action=${ARGS_ACTION} (branch=$BRANCH_ID)"
}

# === Read ===
do_read() {
    local id="${1:-}"
    if [[ -z "$id" ]]; then
        echo "Usage: branch-handoff.sh read <notification_id>"
        exit 1
    fi
    local payload="$HANDOFF_DIR/payloads/${id}.md"
    if [[ -f "$payload" ]]; then
        cat "$payload"
    else
        echo "[handoff] no payload for notification #${id}"
        exit 1
    fi
}

# === Backlog Check ===
do_backlog_check() {
    local backlog_id="${1:-}"
    if [[ -z "$backlog_id" ]]; then
        echo "Usage: branch-handoff.sh backlog-check <backlog_id>"
        exit 1
    fi

    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    for f in "$HANDOFF_DIR/backlog-status/"*.yml; do
        local bl_id bl_branch bl_time
        bl_id=$(grep '^backlog:' "$f" | awk '{print $2}' || echo "")
        bl_branch=$(grep '^branch:' "$f" | awk '{print $2}' || echo "")
        bl_time=$(grep '^started_at:' "$f" | sed 's/started_at: *"//' | sed 's/"//' || echo "")
        if [[ "$bl_id" == "$backlog_id" ]]; then
            echo "BACKLOG-${backlog_id}: FIXING by ${bl_branch} (since ${bl_time})"
            eval "$prev_nullglob" 2>/dev/null || true
            return 1
        fi
    done

    eval "$prev_nullglob" 2>/dev/null || true
    echo "BACKLOG-${backlog_id}: AVAILABLE"
    return 0
}

# === Status ===
do_status() {
    echo "=== Branch Handoff Status ==="
    echo ""

    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    echo "Active branches:"
    local count=0
    for f in "$HANDOFF_DIR/active/"*.yml; do
        local branch status scopes summary
        branch=$(grep '^branch:' "$f" | awk '{print $2}' || echo "")
        status=$(grep '^status:' "$f" | awk '{print $2}' || echo "unknown")
        scopes=$(grep '^  - ' "$f" | sed 's/  - //' | paste -sd',' - || echo "")
        summary=$(grep '^summary:' "$f" | sed 's/summary: *"//' | sed 's/"//' || echo "")
        echo "  ${branch} [${status}] scopes=${scopes} — ${summary}"
        count=$((count + 1))
    done
    if [[ $count -eq 0 ]]; then echo "  (none)"; fi

    echo ""
    echo "Backlog items in progress:"
    count=0
    for f in "$HANDOFF_DIR/backlog-status/"*.yml; do
        local bl_id bl_branch
        bl_id=$(grep '^backlog:' "$f" | awk '{print $2}' || echo "")
        bl_branch=$(grep '^branch:' "$f" | awk '{print $2}' || echo "")
        echo "  BACKLOG-${bl_id} → ${bl_branch}"
        count=$((count + 1))
    done
    if [[ $count -eq 0 ]]; then echo "  (none)"; fi

    echo ""
    local index_count=0
    if [[ -f "$HANDOFF_DIR/index" ]]; then
        index_count=$(wc -l < "$HANDOFF_DIR/index" | tr -d ' ')
    fi
    echo "Total notifications: ${index_count}"

    eval "$prev_nullglob" 2>/dev/null || true
}

# === Cleanup ===
do_cleanup() {
    echo "[handoff] cleanup started"

    local workspace_base
    workspace_base=$(dirname "$PROJECT_ROOT")

    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    # 1. stale active 엔트리 정리 (PID dead AND timeout)
    for f in "$HANDOFF_DIR/active/"*.yml; do
        local branch pid updated_at
        branch=$(grep '^branch:' "$f" | awk '{print $2}' || echo "")
        pid=$(grep '^pid:' "$f" | awk '{print $2}' || echo "")
        updated_at=$(grep '^updated_at:' "$f" | sed 's/updated_at: *"//' | sed 's/"//' || echo "")

        local pid_dead=false
        local timed_out=false

        if [[ -n "$pid" ]] && ! check_pid_alive "$pid"; then
            pid_dead=true
        fi

        if [[ -n "$updated_at" ]]; then
            local updated_epoch now elapsed
            updated_epoch=$(date -d "$updated_at" +%s 2>/dev/null || echo "0")
            now=$(date +%s)
            elapsed=$((now - updated_epoch))
            if [[ $elapsed -ge $STALE_TIMEOUT ]]; then
                timed_out=true
            fi
        fi

        if [[ "$pid_dead" == true && "$timed_out" == true ]]; then
            echo "[handoff] removing stale active entry: $branch"
            rm -f "$f"
            rm -f "$HANDOFF_DIR/backlog-status/${branch}.yml"
        fi
    done

    # 2. 고아 파일 정리 (워크스페이스 디렉토리 존재 여부 기준)
    for f in "$HANDOFF_DIR/watermarks/"*; do
        if [[ ! -f "$f" ]]; then continue; fi
        local branch_name
        branch_name=$(basename "$f")
        if [[ ! -d "${workspace_base}/apex_pipeline_${branch_name}" ]]; then
            echo "[handoff] removing orphan watermark: $branch_name"
            rm -f "$f"
        fi
    done

    for f in "$HANDOFF_DIR/backlog-status/"*.yml; do
        local branch_name
        branch_name=$(basename "$f" .yml)
        if [[ ! -d "${workspace_base}/apex_pipeline_${branch_name}" ]]; then
            echo "[handoff] removing orphan backlog-status: $branch_name"
            rm -f "$f"
        fi
    done

    for f in "$HANDOFF_DIR/active/"*.yml; do
        local branch_name
        branch_name=$(basename "$f" .yml)
        if [[ ! -d "${workspace_base}/apex_pipeline_${branch_name}" ]]; then
            echo "[handoff] removing orphan active: $branch_name"
            rm -f "$f"
        fi
    done

    # responses 내 고아 브랜치 파일 정리
    for d in "$HANDOFF_DIR/responses/"*/; do
        if [[ ! -d "$d" ]]; then continue; fi
        for f in "$d"*.yml; do
            if [[ ! -f "$f" ]]; then continue; fi
            local branch_name
            branch_name=$(basename "$f" .yml)
            if [[ ! -d "${workspace_base}/apex_pipeline_${branch_name}" ]]; then
                echo "[handoff] removing orphan response: $(basename "$d")/$(basename "$f")"
                rm -f "$f"
            fi
        done
        rmdir "$d" 2>/dev/null || true
    done

    # 3. index compaction — 모든 활성 watermark 최솟값 이하를 archive
    local has_watermarks=false
    local min_watermark=999999999
    for f in "$HANDOFF_DIR/watermarks/"*; do
        if [[ ! -f "$f" ]]; then continue; fi
        has_watermarks=true
        local wm
        wm=$(cat "$f" | tr -d '[:space:]')
        wm=${wm:-0}
        (( wm < min_watermark )) && min_watermark=$wm
    done

    if [[ "$has_watermarks" == true && $min_watermark -gt 0 && $min_watermark -lt 999999999 ]]; then
        local tmp_keep tmp_archive
        tmp_keep=$(mktemp)
        tmp_archive=$(mktemp)
        while IFS='|' read -r id rest; do
            if [[ -z "$id" ]]; then continue; fi
            if (( id <= min_watermark )); then
                echo "${id}|${rest}" >> "$tmp_archive"
                if [[ -f "$HANDOFF_DIR/payloads/${id}.md" ]]; then
                    mv "$HANDOFF_DIR/payloads/${id}.md" "$HANDOFF_DIR/archive/" 2>/dev/null || true
                fi
            else
                echo "${id}|${rest}" >> "$tmp_keep"
            fi
        done < "$HANDOFF_DIR/index"

        if [[ -s "$tmp_archive" ]]; then
            cat "$tmp_archive" >> "$HANDOFF_DIR/archive/index.archived"
            mv "$tmp_keep" "$HANDOFF_DIR/index"
            echo "[handoff] compacted: moved entries <= $min_watermark to archive"
        else
            rm -f "$tmp_keep"
        fi
        rm -f "$tmp_archive"
    fi

    eval "$prev_nullglob" 2>/dev/null || true
    echo "[handoff] cleanup complete"
}

# === Main ===
main() {
    local cmd="${1:-help}"
    shift || true

    case "$cmd" in
        init)          do_init ;;
        notify)        do_init; do_notify "$@" ;;
        check)         do_init; do_check "$@" ;;
        ack)           do_init; do_ack "$@" ;;
        read)          do_init; do_read "$@" ;;
        status)        do_init; do_status ;;
        backlog-check) do_init; do_backlog_check "$@" ;;
        cleanup)       do_init; do_cleanup ;;
        # 내부 테스트용
        _lock_index)    do_init; lock_index ;;
        _unlock_index)  unlock_index ;;
        _append_index)  do_init; append_index "$@" ;;
        *)             echo "Usage: branch-handoff.sh <init|notify|check|ack|read|status|backlog-check|cleanup> [args...]"; exit 1 ;;
    esac
}

main "$@"
