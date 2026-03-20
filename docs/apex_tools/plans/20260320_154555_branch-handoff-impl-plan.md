# Branch Handoff System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 브랜치 간 중앙 인수인계 시스템 (`branch-handoff.sh`) 구현 — 파일 기반 3-Tier 알림 + 5단계 게이트

**Architecture:** `queue-lock.sh`와 동일한 패턴의 단일 bash 스크립트. `%LOCALAPPDATA%/apex-branch-handoff/`에 중앙 디렉토리를 두고, index(mkdir 락 보호) + 에이전트별 파일(watermark, active, backlog-status, response)로 동시성 안전하게 운영.

**Tech Stack:** Bash (MSYS2/Git Bash on Windows), `mkdir` atomic lock, YAML-like plaintext

**Spec:** `docs/apex_tools/plans/20260320_152330_branch-handoff-system-design.md`

---

## File Structure

| 파일 | 역할 |
|------|------|
| **Create:** `apex_tools/branch-handoff.sh` | 메인 스크립트 — 전체 CLI |
| **Create:** `apex_tools/tests/test-branch-handoff.sh` | 테스트 스크립트 |
| **Modify:** `CLAUDE.md` | handoff 규칙 추가 |

---

## Task 1: 스크립트 뼈대 + init + 설정

**Files:**
- Create: `apex_tools/branch-handoff.sh`
- Create: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: 테스트 뼈대 작성**

```bash
#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(mktemp -d)"
export APEX_HANDOFF_DIR="$TEST_DIR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HANDOFF="$SCRIPT_DIR/../branch-handoff.sh"

pass=0; fail=0
assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS: $desc"; ((pass++)) || true
    else
        echo "  FAIL: $desc (expected='$expected', actual='$actual')"; ((fail++)) || true
    fi
}
assert_dir_exists()  { if [[ -d "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_file_exists() { if [[ -f "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_no_file()     { if [[ ! -f "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_no_dir()      { if [[ ! -d "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_contains()    { if echo "$2" | grep -q "$1"; then echo "  PASS: $3"; ((pass++)) || true; else echo "  FAIL: $3 (pattern='$1' not in output)"; ((fail++)) || true; fi; }
assert_exit_code()   { if [[ "$1" == "$2" ]]; then echo "  PASS: $3"; ((pass++)) || true; else echo "  FAIL: $3 (expected exit=$1, got=$2)"; ((fail++)) || true; fi; }

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

# ── Test 1: init ──
echo "[Test 1] init으로 디렉토리 생성"
"$HANDOFF" init
assert_file_exists "$TEST_DIR/index" "index 파일 생성"
assert_dir_exists "$TEST_DIR/watermarks" "watermarks/ 생성"
assert_dir_exists "$TEST_DIR/payloads" "payloads/ 생성"
assert_dir_exists "$TEST_DIR/active" "active/ 생성"
assert_dir_exists "$TEST_DIR/responses" "responses/ 생성"
assert_dir_exists "$TEST_DIR/backlog-status" "backlog-status/ 생성"
assert_dir_exists "$TEST_DIR/archive" "archive/ 생성"

echo ""
echo "=========================================="
echo "결과: pass=$pass, fail=$fail"
echo "=========================================="
[[ $fail -eq 0 ]] && exit 0 || exit 1
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: FAIL (스크립트 없음)

- [ ] **Step 3: 메인 스크립트 뼈대 구현**

```bash
#!/usr/bin/env bash
set -euo pipefail

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
    touch "$HANDOFF_DIR/index"
}

# === Main ===
main() {
    local cmd="${1:-help}"
    shift || true

    case "$cmd" in
        init)    do_init ;;
        *)       echo "Usage: branch-handoff.sh <init|notify|check|ack|read|status|backlog-check|cleanup> [args...]"; exit 1 ;;
    esac
}

main "$@"
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: 7 PASS, 0 FAIL

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff 스크립트 뼈대 + init 구현"
```

---

## Task 2: index 락킹 (mkdir lock)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: index 락 테스트 추가**

테스트 파일 끝(결과 출력 앞)에 추가:

```bash
# ── Test 2: index lock 획득/해제 ──
echo "[Test 2] index lock 획득/해제"
"$HANDOFF" _lock_index
assert_dir_exists "$TEST_DIR/index.lock" "index.lock 디렉토리 생성"
assert_file_exists "$TEST_DIR/index.lock/owner" "owner 파일 생성"

owner_pid=$(grep '^PID=' "$TEST_DIR/index.lock/owner" | cut -d= -f2)
assert_eq "owner PID 기록됨" "$$" "$owner_pid"

"$HANDOFF" _unlock_index
assert_no_dir "$TEST_DIR/index.lock" "index.lock 디렉토리 삭제"

# ── Test 3: stale index lock 감지 ──
echo "[Test 3] stale index lock 감지 (dead PID)"
mkdir -p "$TEST_DIR/index.lock"
printf "PID=99999\nACQUIRED=$(date +%s)\n" > "$TEST_DIR/index.lock/owner"
"$HANDOFF" _lock_index 2>/dev/null
assert_dir_exists "$TEST_DIR/index.lock" "stale 제거 후 재획득"
"$HANDOFF" _unlock_index

# ── Test 4: append_index 원자적 ID 생성 ──
echo "[Test 4] append_index 원자적 ID 생성"
> "$TEST_DIR/index"  # 비우기
result=$("$HANDOFF" _append_index "tier1" "branch_01" "core,shared" "테스트 알림 1")
assert_eq "첫 ID는 1" "1" "$result"

result=$("$HANDOFF" _append_index "tier2" "branch_02" "gateway" "테스트 알림 2")
assert_eq "두 번째 ID는 2" "2" "$result"

line_count=$(wc -l < "$TEST_DIR/index" | tr -d ' ')
assert_eq "index에 2줄" "2" "$line_count"

first_line=$(head -1 "$TEST_DIR/index")
assert_contains "1|tier1|branch_01|core,shared|테스트 알림 1" "$first_line" "첫 줄 포맷 정확"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: Test 2~4 FAIL

- [ ] **Step 3: lock/unlock/append_index 구현**

`branch-handoff.sh`의 `do_init()` 아래에 추가:

```bash
# === Index Locking ===
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
    local new_id=$((last_id + 1))
    echo "${new_id}|${tier}|${branch}|${scopes}|${summary}" >> "$HANDOFF_DIR/index"
    unlock_index
    echo "$new_id"
}
```

`main()` case 문에 내부 테스트용 명령 추가:

```bash
        _lock_index)    do_init; lock_index ;;
        _unlock_index)  unlock_index ;;
        _append_index)  do_init; append_index "$@" ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: 전체 PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff index 락킹 구현 (mkdir atomic lock)"
```

---

## Task 3: notify start (Tier 1)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: notify start 테스트 추가**

```bash
# ── Test 5: notify start ──
echo "[Test 5] notify start — Tier 1 알림"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소"

assert_file_exists "$TEST_DIR/active/branch_03.yml" "active 파일 생성"
assert_file_exists "$TEST_DIR/backlog-status/branch_03.yml" "backlog-status 파일 생성"

index_line=$(cat "$TEST_DIR/index")
assert_contains "tier1" "$index_line" "index에 tier1 기록"
assert_contains "branch_03" "$index_line" "index에 branch_id 기록"
assert_contains "core,shared" "$index_line" "index에 scopes 기록"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "status: started" "$active_content" "active status=started"
assert_contains "backlog: 89" "$active_content" "active에 backlog ID"

backlog_content=$(cat "$TEST_DIR/backlog-status/branch_03.yml")
assert_contains "backlog: 89" "$backlog_content" "backlog-status에 ID"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: notify start 구현**

`branch-handoff.sh`에 추가:

```bash
# === Argument Parsing Helper ===
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
        [[ -n "$ARGS_BACKLOG" ]] && echo "backlog: $ARGS_BACKLOG"
        echo "status: started"
        echo "scopes:"
        IFS=',' read -ra scope_arr <<< "$ARGS_SCOPES"
        for s in "${scope_arr[@]}"; do echo "  - $s"; done
        echo "summary: \"$ARGS_SUMMARY\""
        echo "started_at: \"$now\""
        echo "updated_at: \"$now\""
    } > "$HANDOFF_DIR/active/${BRANCH_ID}.yml"

    # backlog-status 파일 생성 (백로그 있을 때만)
    if [[ -n "$ARGS_BACKLOG" ]]; then
        {
            echo "backlog: $ARGS_BACKLOG"
            echo "branch: $BRANCH_ID"
            echo "started_at: \"$now\""
        } > "$HANDOFF_DIR/backlog-status/${BRANCH_ID}.yml"
    fi

    # index에 추가
    local summary_text="$ARGS_SUMMARY"
    [[ -n "$ARGS_BACKLOG" ]] && summary_text="BACKLOG-${ARGS_BACKLOG} $summary_text"

    local new_id
    new_id=$(append_index "tier1" "$BRANCH_ID" "$ARGS_SCOPES" "$summary_text")

    echo "[handoff] Tier 1 notification published: #${new_id} (branch=$BRANCH_ID, scopes=$ARGS_SCOPES)"
}
```

`main()` case에 추가:

```bash
        notify)  do_init; do_notify "$@" ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff notify start (Tier 1) 구현"
```

---

## Task 4: notify design (Tier 2) + SUPERSEDE

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: notify design 테스트 추가**

```bash
# ── Test 6: notify design ──
echo "[Test 6] notify design — Tier 2 알림"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/payloads/"*

# 먼저 active 파일 생성 (notify start로)
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소"

# payload 파일 준비
echo "## 테스트 설계 내용" > "$TEST_DIR/tmp_payload.md"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared,gateway --summary "ErrorCode 분리" \
  --payload-file "$TEST_DIR/tmp_payload.md"

assert_file_exists "$TEST_DIR/payloads/2.md" "payload 파일 생성"

payload_content=$(cat "$TEST_DIR/payloads/2.md")
assert_contains "tier: tier2" "$payload_content" "payload에 tier 기록"
assert_contains "branch: branch_03" "$payload_content" "payload에 branch 기록"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "status: designing" "$active_content" "active status→designing"
assert_contains "latest_tier2_id: 2" "$active_content" "active에 latest_tier2_id"

# ── Test 7: notify design 재발행 (SUPERSEDE) ──
echo "[Test 7] notify design 재발행 — SUPERSEDE"
echo "## 변경된 설계" > "$TEST_DIR/tmp_payload2.md"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared,gateway --summary "방향 변경" \
  --payload-file "$TEST_DIR/tmp_payload2.md"

index_last=$(tail -1 "$TEST_DIR/index")
assert_contains "SUPERSEDE:2" "$index_last" "index에 SUPERSEDE 태그"

assert_file_exists "$TEST_DIR/payloads/3.md" "새 payload 파일 생성"
payload3=$(cat "$TEST_DIR/payloads/3.md")
assert_contains "supersedes: 2" "$payload3" "payload에 supersedes 필드"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "latest_tier2_id: 3" "$active_content" "latest_tier2_id 갱신"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: notify design 구현**

```bash
do_notify_design() {
    local now
    now=$(date +"%Y-%m-%d %H:%M:%S")

    # 이전 Tier 2 ID 찾기 (SUPERSEDE용)
    local prev_tier2_id=""
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        prev_tier2_id=$(grep '^latest_tier2_id:' "$HANDOFF_DIR/active/${BRANCH_ID}.yml" | awk '{print $2}')
    fi

    # summary에 SUPERSEDE 태그 추가
    local summary_text="$ARGS_SUMMARY"
    [[ -n "$prev_tier2_id" ]] && summary_text="[SUPERSEDE:${prev_tier2_id}] $summary_text"

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
        [[ -n "$prev_tier2_id" ]] && echo "supersedes: $prev_tier2_id"
        echo "timestamp: \"$now\""
        echo "---"
        echo ""
        if [[ -n "$ARGS_PAYLOAD_FILE" && -f "$ARGS_PAYLOAD_FILE" ]]; then
            cat "$ARGS_PAYLOAD_FILE"
        fi
    } > "$HANDOFF_DIR/payloads/${new_id}.md"

    # active 파일 갱신
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        sed -i "s/^status: .*/status: designing/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        # latest_tier2_id 갱신 또는 추가
        if grep -q '^latest_tier2_id:' "$HANDOFF_DIR/active/${BRANCH_ID}.yml"; then
            sed -i "s/^latest_tier2_id: .*/latest_tier2_id: $new_id/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        else
            echo "latest_tier2_id: $new_id" >> "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        fi
        sed -i "s/^updated_at: .*/updated_at: \"$now\"/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
    fi

    # scopes 갱신 (확장될 수 있으므로)
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        # scopes 블록 교체 — sed로 scopes: 줄부터 다음 비-indent 줄 전까지 교체
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
                    continue  # 기존 scope 줄 스킵
                else
                    in_scopes=false
                fi
            fi
            echo "$line"
        done < "$HANDOFF_DIR/active/${BRANCH_ID}.yml" > "$tmp_file"
        mv "$tmp_file" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
    fi

    echo "[handoff] Tier 2 notification published: #${new_id} (branch=$BRANCH_ID, scopes=$ARGS_SCOPES)"
    [[ -n "$prev_tier2_id" ]] && echo "[handoff] supersedes: #${prev_tier2_id}"
}
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff notify design (Tier 2 + SUPERSEDE) 구현"
```

---

## Task 5: notify merge (Tier 3)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: notify merge 테스트 추가**

```bash
# ── Test 8: notify merge ──
echo "[Test 8] notify merge — Tier 3 알림 + 정리"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"*

# setup: start → design → merge
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify start --backlog 99 --scopes ci --summary "Valgrind fix"
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify design --scopes ci --summary "Valgrind 필터 수정"

assert_file_exists "$TEST_DIR/active/branch_01.yml" "merge 전 active 존재"
assert_file_exists "$TEST_DIR/backlog-status/branch_01.yml" "merge 전 backlog-status 존재"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify merge --summary "BACKLOG-99 머지 완료"

assert_no_file "$TEST_DIR/active/branch_01.yml" "merge 후 active 삭제"
assert_no_file "$TEST_DIR/backlog-status/branch_01.yml" "merge 후 backlog-status 삭제"

index_last=$(tail -1 "$TEST_DIR/index")
assert_contains "tier3" "$index_last" "index에 tier3 기록"
assert_contains "branch_01" "$index_last" "index에 branch 기록"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: notify merge 구현**

```bash
do_notify_merge() {
    # scopes는 active 파일에서 추출
    local scopes=""
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        scopes=$(grep '^  - ' "$HANDOFF_DIR/active/${BRANCH_ID}.yml" | sed 's/  - //' | paste -sd',' -)
    fi
    [[ -z "$scopes" ]] && scopes="unknown"

    # 머지 커밋 해시 추출 시도
    local commit_hash=""
    commit_hash=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo "")
    local summary_text="$ARGS_SUMMARY"
    [[ -n "$commit_hash" ]] && summary_text="$summary_text [$commit_hash]"

    # index에 추가 (페이로드 없음)
    local new_id
    new_id=$(append_index "tier3" "$BRANCH_ID" "$scopes" "$summary_text")

    # active 파일 삭제
    rm -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml"

    # backlog-status 파일 삭제
    rm -f "$HANDOFF_DIR/backlog-status/${BRANCH_ID}.yml"

    echo "[handoff] Tier 3 notification published: #${new_id} (branch=$BRANCH_ID, merge complete)"
}
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff notify merge (Tier 3) 구현"
```

---

## Task 6: check (기본 폴링 + 스코프 필터링)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: check 테스트 추가**

```bash
# ── Test 9: check — 새 알림 없음 ──
echo "[Test 9] check — 새 알림 없음"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/watermarks/"*

# watermark를 0으로 설정하고, index도 비어있으면 → 알림 없음
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" "$HANDOFF" check 2>&1)
assert_contains "No new notifications" "$output" "빈 index에서 알림 없음"

# ── Test 10: check — 새 알림 있음 ──
echo "[Test 10] check — 새 알림 발견"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/watermarks/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "테스트"

output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" "$HANDOFF" check 2>&1)
assert_contains "#1" "$output" "알림 #1 표시"
assert_contains "tier1" "$output" "tier1 표시"

# ── Test 11: check --scopes 필터링 ──
echo "[Test 11] check --scopes 필터링"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/watermarks/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier1" "branch_03" "core,shared" "코어 작업" > /dev/null
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" _append_index "tier1" "branch_01" "ci" "CI 작업" > /dev/null

# gateway 스코프로 필터 → 겹치는 거 없어야 함
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_02" "$HANDOFF" check --scopes gateway 2>&1)
assert_contains "No new notifications" "$output" "gateway 스코프 필터 → 매칭 없음"

# core 스코프로 필터 → 1건 매칭
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_02" "$HANDOFF" check --scopes core 2>&1)
assert_contains "#1" "$output" "core 스코프 필터 → #1 매칭"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: check 구현**

```bash
# === Check ===
do_check() {
    parse_args "$@"

    # SUPERSEDE 맵 구축: supersede된 ID 목록
    local -A superseded_ids
    while IFS='|' read -r id tier branch scopes summary; do
        [[ -z "$id" ]] && continue
        local sup_id
        sup_id=$(echo "$summary" | sed -n 's/.*\[SUPERSEDE:\([0-9]*\)\].*/\1/p')
        [[ -n "$sup_id" ]] && superseded_ids[$sup_id]=1
    done < "$HANDOFF_DIR/index"

    # 새 알림 수집
    local found=0
    local output_lines=""

    while IFS='|' read -r id tier branch scopes summary; do
        [[ -z "$id" ]] && continue
        [[ "$branch" == "$BRANCH_ID" ]] && continue  # 자기 알림은 스킵
        [[ -n "${superseded_ids[$id]:-}" ]] && continue  # supersede된 건 스킵

        # 스코프 필터링
        if [[ -n "${ARGS_SCOPES:-}" ]]; then
            local match=false
            IFS=',' read -ra filter_scopes <<< "$ARGS_SCOPES"
            IFS=',' read -ra notif_scopes <<< "$scopes"
            for fs in "${filter_scopes[@]}"; do
                for ns in "${notif_scopes[@]}"; do
                    [[ "$fs" == "$ns" ]] && match=true
                done
            done
            [[ "$match" == false ]] && continue
        fi

        # 이미 ack한 건 스킵
        if [[ -f "$HANDOFF_DIR/responses/${id}/${BRANCH_ID}.yml" ]]; then
            continue
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
        [[ -n "$payload_info" ]] && output_lines+="     ${payload_info}"$'\n'
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
        echo "$output_lines"
        echo "Resolve all before proceeding."
        return 1
    else
        echo "${found} new notification(s):"
        echo ""
        echo "$output_lines"
        return 0
    fi
}
```

`main()` case에 추가:

```bash
        check)   do_init; do_check "$@" ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff check (폴링 + 스코프 필터 + SUPERSEDE 처리) 구현"
```

---

## Task 7: check --gate (게이트 시스템)

**Files:**
- Modify: `apex_tools/tests/test-branch-handoff.sh`

Note: `check --gate` 로직은 Task 6의 `do_check()`에 이미 포함됨. 이 태스크는 게이트 동작 검증에 집중.

- [ ] **Step 1: 게이트 테스트 추가**

```bash
# ── Test 12: check --gate — 차단 ──
echo "[Test 12] check --gate — 미응답 시 차단"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/watermarks/"* "$TEST_DIR/responses/"* "$TEST_DIR/active/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core,shared" "ErrorCode 분리" > /dev/null

# branch_01의 active가 core 스코프
mkdir -p "$TEST_DIR/active"
echo -e "branch: branch_01\nscopes:\n  - core" > "$TEST_DIR/active/branch_01.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "게이트 차단 시 exit code 1" "1" "$exit_code"
assert_contains "GATE:design" "$output" "GATE:design 출력"
assert_contains "BLOCKED" "$output" "BLOCKED 출력"

# ── Test 13: check --gate — 클리어 ──
echo "[Test 13] check --gate — ack 후 클리어"
mkdir -p "$TEST_DIR/responses/1"
echo "action: no-impact" > "$TEST_DIR/responses/1/branch_01.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "게이트 클리어 시 exit code 0" "0" "$exit_code"
assert_contains "CLEAR" "$output" "CLEAR 출력"

# ── Test 14: SUPERSEDE된 알림은 게이트에서 제외 ──
echo "[Test 14] SUPERSEDE된 알림 — 게이트 자동 dismiss"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/responses/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "원본 설계" > /dev/null
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "[SUPERSEDE:1] 변경된 설계" > /dev/null

# #1은 supersede됨 → 게이트는 #2만 블록해야 함
exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "SUPERSEDE: 1건만 블록" "1" "$exit_code"
assert_contains "#2" "$output" "#2만 표시"
```

- [ ] **Step 2: 테스트 실행 — 통과 확인**

이미 Task 6에서 구현된 로직으로 통과해야 함. 실패하면 `do_check()`의 SUPERSEDE 파싱 또는 exit code 처리를 수정.

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/tests/test-branch-handoff.sh
git commit -m "test(tools): branch-handoff 게이트 시스템 검증 테스트 추가"
```

---

## Task 8: ack (대응 선언 + 워터마크 갱신)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: ack 테스트 추가**

```bash
# ── Test 15: ack ──
echo "[Test 15] ack — 대응 선언 + 워터마크 갱신"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/responses/"* "$TEST_DIR/watermarks/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "테스트" > /dev/null

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 1 --action no-impact --reason "영향 없음"

assert_file_exists "$TEST_DIR/responses/1/branch_01.yml" "response 파일 생성"

response=$(cat "$TEST_DIR/responses/1/branch_01.yml")
assert_contains "action: no-impact" "$response" "action 기록"
assert_contains "reason:" "$response" "reason 기록"

# ── Test 16: ack 후 check --gate 클리어 ──
echo "[Test 16] ack 후 게이트 클리어"
exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate build --scopes core 2>&1) || exit_code=$?
assert_eq "ack 후 게이트 클리어" "0" "$exit_code"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: ack 구현**

```bash
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
```

`main()` case에 추가:

```bash
        ack)     do_init; do_ack "$@" ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff ack (대응 선언 + 워터마크) 구현"
```

---

## Task 9: read + status + backlog-check

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: read/status/backlog-check 테스트 추가**

```bash
# ── Test 17: read ──
echo "[Test 17] read — payload 출력"
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" read 2 2>&1) || true
assert_contains "tier: tier2" "$output" "payload 내용 출력"

# ── Test 18: backlog-check — FIXING ──
echo "[Test 18] backlog-check"
rm -f "$TEST_DIR/backlog-status/"*
mkdir -p "$TEST_DIR/backlog-status"
echo -e "backlog: 89\nbranch: branch_03\nstarted_at: \"2026-03-20 14:30:00\"" \
  > "$TEST_DIR/backlog-status/branch_03.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 89 2>&1) || exit_code=$?
assert_eq "FIXING 시 exit 1" "1" "$exit_code"
assert_contains "FIXING" "$output" "FIXING 출력"
assert_contains "branch_03" "$output" "소유 브랜치 표시"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 42 2>&1) || exit_code=$?
assert_eq "AVAILABLE 시 exit 0" "0" "$exit_code"
assert_contains "AVAILABLE" "$output" "AVAILABLE 출력"

# ── Test 19: status ──
echo "[Test 19] status — 전체 현황"
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" status 2>&1)
assert_contains "branch_03" "$output" "활성 브랜치 표시"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: read/status/backlog-check 구현**

```bash
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
        bl_id=$(grep '^backlog:' "$f" | awk '{print $2}')
        bl_branch=$(grep '^branch:' "$f" | awk '{print $2}')
        bl_time=$(grep '^started_at:' "$f" | sed 's/started_at: *"//' | sed 's/"//')
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

    # 활성 브랜치
    echo "Active branches:"
    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    local count=0
    for f in "$HANDOFF_DIR/active/"*.yml; do
        local branch status scopes summary
        branch=$(grep '^branch:' "$f" | awk '{print $2}')
        status=$(grep '^status:' "$f" | awk '{print $2}')
        scopes=$(grep '^  - ' "$f" | sed 's/  - //' | paste -sd',' -)
        summary=$(grep '^summary:' "$f" | sed 's/summary: *"//' | sed 's/"//')
        echo "  ${branch} [${status}] scopes=${scopes} — ${summary}"
        count=$((count + 1))
    done
    [[ $count -eq 0 ]] && echo "  (none)"

    echo ""
    echo "Backlog items in progress:"
    count=0
    for f in "$HANDOFF_DIR/backlog-status/"*.yml; do
        local bl_id bl_branch
        bl_id=$(grep '^backlog:' "$f" | awk '{print $2}')
        bl_branch=$(grep '^branch:' "$f" | awk '{print $2}')
        echo "  BACKLOG-${bl_id} → ${bl_branch}"
        count=$((count + 1))
    done
    [[ $count -eq 0 ]] && echo "  (none)"

    echo ""
    local index_count
    index_count=$(wc -l < "$HANDOFF_DIR/index" | tr -d ' ')
    echo "Total notifications: ${index_count}"

    eval "$prev_nullglob" 2>/dev/null || true
}
```

`main()` case에 추가:

```bash
        read)          do_init; do_read "$@" ;;
        status)        do_init; do_status ;;
        backlog-check) do_init; do_backlog_check "$@" ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff read/status/backlog-check 구현"
```

---

## Task 10: cleanup (stale 감지 + 고아 정리 + compaction)

**Files:**
- Modify: `apex_tools/branch-handoff.sh`
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: cleanup 테스트 추가**

```bash
# ── Test 20: cleanup — stale active 감지 ──
echo "[Test 20] cleanup — stale active 감지"
mkdir -p "$TEST_DIR/active"
echo -e "branch: branch_99\npid: 99999\nstatus: implementing\nscopes:\n  - core\nsummary: \"test\"\nstarted_at: \"2026-03-19 10:00:00\"\nupdated_at: \"2026-03-19 10:00:00\"" \
  > "$TEST_DIR/active/branch_99.yml"

# APEX_HANDOFF_STALE_SEC=0으로 즉시 stale 판정
APEX_HANDOFF_DIR="$TEST_DIR" APEX_HANDOFF_STALE_SEC=0 "$HANDOFF" cleanup 2>/dev/null
assert_no_file "$TEST_DIR/active/branch_99.yml" "stale active 제거"

# ── Test 21: cleanup — 고아 watermark 제거 ──
echo "[Test 21] cleanup — 고아 watermark 제거"
echo "5" > "$TEST_DIR/watermarks/branch_99"

# branch_99 워크스페이스가 없으므로 고아
APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" cleanup 2>/dev/null
assert_no_file "$TEST_DIR/watermarks/branch_99" "고아 watermark 제거"
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

- [ ] **Step 3: cleanup 구현**

```bash
# === Cleanup ===
do_cleanup() {
    echo "[handoff] cleanup started"

    local workspace_base
    workspace_base=$(dirname "$PROJECT_ROOT")

    local prev_nullglob
    prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    # 1. stale active 엔트리 정리
    for f in "$HANDOFF_DIR/active/"*.yml; do
        local branch pid updated_at
        branch=$(grep '^branch:' "$f" | awk '{print $2}')
        pid=$(grep '^pid:' "$f" | awk '{print $2}')
        updated_at=$(grep '^updated_at:' "$f" | sed 's/updated_at: *"//' | sed 's/"//')

        local pid_dead=false
        local timed_out=false

        # PID 확인
        if [[ -n "$pid" ]] && ! check_pid_alive "$pid"; then
            pid_dead=true
        fi

        # 타임스탬프 확인
        if [[ -n "$updated_at" ]]; then
            local updated_epoch now elapsed
            updated_epoch=$(date -d "$updated_at" +%s 2>/dev/null || echo "0")
            now=$(date +%s)
            elapsed=$((now - updated_epoch))
            if [[ $elapsed -ge $STALE_TIMEOUT ]]; then
                timed_out=true
            fi
        fi

        # 둘 다 해당해야 stale (PID dead AND timeout)
        if [[ "$pid_dead" == true && "$timed_out" == true ]]; then
            echo "[handoff] removing stale active entry: $branch"
            rm -f "$f"
            rm -f "$HANDOFF_DIR/backlog-status/${branch}.yml"
        fi
    done

    # 2. 고아 파일 정리 (워크스페이스 디렉토리 존재 여부 기준)
    for f in "$HANDOFF_DIR/watermarks/"*; do
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
        [[ ! -d "$d" ]] && continue
        for f in "$d"*.yml; do
            [[ ! -f "$f" ]] && continue
            local branch_name
            branch_name=$(basename "$f" .yml)
            if [[ ! -d "${workspace_base}/apex_pipeline_${branch_name}" ]]; then
                echo "[handoff] removing orphan response: $f"
                rm -f "$f"
            fi
        done
        # 빈 디렉토리 정리
        rmdir "$d" 2>/dev/null || true
    done

    # 3. index compaction — 모든 watermark 최솟값 이하 줄을 archive로 이동
    local min_watermark=999999999
    for f in "$HANDOFF_DIR/watermarks/"*; do
        local wm
        wm=$(cat "$f" | tr -d '[:space:]')
        wm=${wm:-0}
        (( wm < min_watermark )) && min_watermark=$wm
    done

    if [[ $min_watermark -gt 0 && $min_watermark -lt 999999999 ]]; then
        local tmp_keep tmp_archive
        tmp_keep=$(mktemp)
        tmp_archive=$(mktemp)
        while IFS='|' read -r id rest; do
            [[ -z "$id" ]] && continue
            if (( id <= min_watermark )); then
                echo "${id}|${rest}" >> "$tmp_archive"
                # 해당 payload도 archive로
                [[ -f "$HANDOFF_DIR/payloads/${id}.md" ]] && \
                    mv "$HANDOFF_DIR/payloads/${id}.md" "$HANDOFF_DIR/archive/" 2>/dev/null || true
            else
                echo "${id}|${rest}" >> "$tmp_keep"
            fi
        done < "$HANDOFF_DIR/index"

        if [[ -s "$tmp_archive" ]]; then
            cat "$tmp_archive" >> "$HANDOFF_DIR/archive/index.archived"
            mv "$tmp_keep" "$HANDOFF_DIR/index"
            echo "[handoff] compacted: moved entries ≤ $min_watermark to archive"
        fi
        rm -f "$tmp_keep" "$tmp_archive"
    fi

    eval "$prev_nullglob" 2>/dev/null || true
    echo "[handoff] cleanup complete"
}
```

`main()` case에 추가:

```bash
        cleanup) do_init; do_cleanup ;;
```

- [ ] **Step 4: 테스트 실행 — 통과 확인**

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/branch-handoff.sh apex_tools/tests/test-branch-handoff.sh
git commit -m "feat(tools): branch-handoff cleanup (stale 감지 + 고아 정리 + compaction) 구현"
```

---

## Task 11: 통합 테스트 (멀티 브랜치 시나리오)

**Files:**
- Modify: `apex_tools/tests/test-branch-handoff.sh`

- [ ] **Step 1: 전체 라이프사이클 통합 테스트 추가**

```bash
# ── Test 22: 전체 라이프사이클 ──
echo "[Test 22] 전체 라이프사이클 — start → design → gate → ack → merge"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"* "$TEST_DIR/responses/"* "$TEST_DIR/watermarks/"* "$TEST_DIR/payloads/"*

# branch_03이 작업 시작
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소" 2>/dev/null

# branch_01이 게이트 체크 → 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 1에 의해 차단" "1" "$exit_code"

# branch_01이 ack
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 1 --action no-impact --reason "영향 없음" 2>/dev/null

# branch_03이 설계 발행
echo "## 설계 내용" > "$TEST_DIR/tmp_design.md"
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared --summary "설계 확정" \
  --payload-file "$TEST_DIR/tmp_design.md" 2>/dev/null

# branch_01이 게이트 체크 → 새 Tier 2에 의해 다시 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate plan --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 2에 의해 재차단" "1" "$exit_code"

# branch_01이 Tier 2 ack
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 2 --action will-rebase --reason "다음 rebase에 반영" 2>/dev/null

# branch_03이 머지
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify merge --summary "BACKLOG-89 완료" 2>/dev/null

# Tier 3에 의해 다시 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate build --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 3에 의해 차단" "1" "$exit_code"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 3 --action rebased --reason "rebase 완료" 2>/dev/null

# 최종 게이트 클리어
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate merge --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: 전부 ack 후 클리어" "0" "$exit_code"

# 정리 확인
assert_no_file "$TEST_DIR/active/branch_03.yml" "머지 후 active 삭제됨"
assert_no_file "$TEST_DIR/backlog-status/branch_03.yml" "머지 후 backlog-status 삭제됨"

# ── Test 23: backlog 중복 착수 방지 ──
echo "[Test 23] backlog 중복 착수 방지"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/backlog-status/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify start --backlog 42 --scopes gateway --summary "테스트" 2>/dev/null

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 42 2>&1) || exit_code=$?
assert_eq "이미 착수된 백로그 → exit 1" "1" "$exit_code"
assert_contains "FIXING" "$output" "FIXING 표시"
```

- [ ] **Step 2: 테스트 실행 — 전체 통과 확인**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: 전체 PASS

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/tests/test-branch-handoff.sh
git commit -m "test(tools): branch-handoff 전체 라이프사이클 통합 테스트"
```

---

## Task 12: CLAUDE.md 규칙 추가

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: 루트 CLAUDE.md에 handoff 규칙 추가**

`### Git / 브랜치` 섹션 끝(또는 `### 설계 원칙` 바로 앞)에 새 서브섹션 추가:

```markdown
### 브랜치 인수인계 (Branch Handoff)
- **인수인계 도구**: `"<프로젝트루트절대경로>/apex_tools/branch-handoff.sh"` — 브랜치 간 파일 기반 소통 시스템
- **Tier 1 (사전 알림)**: 명확한 작업 목적으로 브랜치 생성 시 `notify start` 실행. 탐색적 작업은 Tier 2로 직행
- **Tier 2 (중간 알림)**: 설계 문서 또는 구현 계획 확정 시 `notify design` 실행. 방향 변경 시 재발행 가능
- **Tier 3 (사후 알림)**: 머지 완료 시 `notify merge` 실행
- **게이트 체크포인트 (전부 차단)**: 설계 완료(`--gate design`), 구현 계획 완료(`--gate plan`), 구현 시작(`--gate implement`), 빌드 전(`--gate build`), 머지 전(`--gate merge`)
- **게이트 차단 시**: 에이전트가 자율적으로 payload 확인 → 영향 판단 → 필요 시 설계/계획 수정 → ack 후 재확인 (유저 개입 불필요)
- **ack 필수**: 대응 선언(`--action`) 없이 무시 금지. 가능한 action: `no-impact`, `will-rebase`, `rebased`, `design-adjusted`, `deferred`
- **백로그 연동**: 백로그 착수 시 자동으로 `backlog-status/`에 등록. 타 에이전트는 `backlog-check`으로 중복 착수 방지
```

- [ ] **Step 2: 커밋**

```bash
git add CLAUDE.md
git commit -m "docs: CLAUDE.md에 branch-handoff 규칙 추가"
```

---

## Task 13: 최종 테스트 + 푸시

- [ ] **Step 1: 전체 테스트 실행**

Run: `bash apex_tools/tests/test-branch-handoff.sh`
Expected: 전체 PASS, 0 FAIL

- [ ] **Step 2: queue-lock 기존 테스트도 깨지지 않았는지 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: 기존 전체 PASS

- [ ] **Step 3: 푸시**

```bash
git push
```
