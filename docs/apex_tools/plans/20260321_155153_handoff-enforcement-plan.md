# 핸드오프 시스템 강제화 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 브랜치 핸드오프 notify를 코드 레벨에서 강제하여, 미등록 에이전트의 작업을 차단한다.

**Architecture:** 기존 4개 파일(branch-handoff.sh, handoff-probe.sh, validate-handoff.sh, session-context.sh)을 수정하여 상태 머신 기반 게이트를 삽입한다. active 파일의 status 필드가 상태 머신을 따르며, 각 hook이 status를 읽어 권한 매트릭스에 따라 차단한다.

**Spec:** `docs/apex_tools/plans/20260321_154910_handoff-enforcement.md`

**Tech Stack:** Bash, Claude Code PreToolUse hooks, 파일 기반 IPC

---

## 파일 구조

| 파일 | 역할 | 변경 |
|------|------|------|
| `apex_tools/branch-handoff.sh` | 핸드오프 CLI | notify plan 추가, --skip-design, status 검증, summary 필수, merge 시 watermark 정리 |
| `.claude/hooks/handoff-probe.sh` | Edit/Write hook | active 등록 + status 기반 전면 차단 |
| `.claude/hooks/validate-handoff.sh` | Bash hook | git commit 시 active 등록 차단 |
| `apex_tools/session-context.sh` | SessionStart | 경고 메시지 강화 (차단 예고) |

---

### Task 1: branch-handoff.sh — 상태 머신 + 새 커맨드

**Files:**
- Modify: `apex_tools/branch-handoff.sh:107-141` (parse_args — --skip-design 플래그)
- Modify: `apex_tools/branch-handoff.sh:143-196` (do_notify, do_notify_start — status/skip-design)
- Modify: `apex_tools/branch-handoff.sh:198-268` (do_notify_design — status 검증)
- Modify: `apex_tools/branch-handoff.sh:270-295` (do_notify_merge — watermark 정리)
- Add function: `do_notify_plan()` (design-notified → implementing)
- Add function: `validate_status()` (상태 전환 검증 헬퍼)

- [ ] **Step 1: --skip-design 플래그 + summary 필수 검증 추가**

`parse_args`에 `ARGS_SKIP_DESIGN` 추가:
```bash
# parse_args 내부에 추가
ARGS_SKIP_DESIGN=""

# case 블록에 추가
--skip-design)   ARGS_SKIP_DESIGN="true"; shift ;;
```

summary 필수 검증 헬퍼 추가:
```bash
require_summary() {
    if [[ -z "${ARGS_SUMMARY:-}" ]]; then
        echo "[handoff] ERROR: --summary is required" >&2
        exit 1
    fi
}
```

- [ ] **Step 2: validate_status 헬퍼 함수 추가**

`do_notify()` 직전에 삽입:
```bash
# === Status Validation ===
get_active_status() {
    local active_file="$HANDOFF_DIR/active/${BRANCH_ID}.yml"
    if [[ ! -f "$active_file" ]]; then
        echo ""
        return
    fi
    grep '^status:' "$active_file" | awk '{print $2}'
}

validate_status() {
    local required="$1" current
    current=$(get_active_status)
    if [[ "$current" != "$required" ]]; then
        echo "[handoff] ERROR: status must be '$required' but is '${current:-unregistered}'" >&2
        exit 1
    fi
}
```

- [ ] **Step 3: do_notify_start 수정 — --skip-design 지원**

`do_notify_start()` 내 status 설정 부분 수정:
```bash
# 기존: echo "status: started"
# 변경:
local initial_status="started"
if [[ "${ARGS_SKIP_DESIGN:-}" == "true" ]]; then
    initial_status="implementing"
fi
```

active 파일 생성 시 `echo "status: $initial_status"` 사용.
`echo "session_pid: $PPID"` 도 active 파일에 추가 (stale 정리용).

`require_summary` 호출을 함수 시작에 추가.

- [ ] **Step 4: do_notify_design 수정 — status 검증 추가**

함수 시작에 추가:
```bash
require_summary
validate_status "started"
```

active 파일 갱신 시 기존 `sed -i "s/^status: .*/status: designing/"` 을 `sed -i "s/^status: .*/status: design-notified/"` 로 변경.

- [ ] **Step 5: do_notify_plan 함수 신설**

```bash
do_notify_plan() {
    parse_args "$@"
    require_summary
    validate_status "design-notified"

    local now
    now=$(date +"%Y-%m-%d %H:%M:%S")

    # active 파일 status 갱신
    if [[ -f "$HANDOFF_DIR/active/${BRANCH_ID}.yml" ]]; then
        sed -i "s/^status: .*/status: implementing/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
        sed -i "s/^updated_at: .*/updated_at: \"$now\"/" "$HANDOFF_DIR/active/${BRANCH_ID}.yml"
    fi

    echo "[handoff] status → implementing (branch=$BRANCH_ID)"
}
```

`do_notify()` switch에 `plan)` 케이스 추가:
```bash
plan)    do_notify_plan "$@" ;;
```

usage 메시지도 갱신: `<start|design|plan|merge>`

- [ ] **Step 6: do_notify_merge 수정 — watermark 정리**

active/backlog-status 삭제 직후 (기존 `rm -f` 라인 인접)에 추가:
```bash
# watermark 파일 삭제 (stale watermark 방지)
rm -f "$HANDOFF_DIR/watermarks/${BRANCH_ID}"
```

- [ ] **Step 7: 수동 검증**

```bash
# 테스트 준비
HANDOFF_SH="$(pwd)/apex_tools/branch-handoff.sh"

# 1. summary 필수 검증
bash "$HANDOFF_SH" notify start --scopes tools
# Expected: ERROR: --summary is required

# 2. notify start (정상)
bash "$HANDOFF_SH" notify start --scopes tools --summary "테스트"
bash "$HANDOFF_SH" status
# Expected: status=started

# 3. notify design (status 검증)
bash "$HANDOFF_SH" notify design --scopes tools --summary "설계 완료"
bash "$HANDOFF_SH" status
# Expected: status=design-notified

# 4. notify plan (status 검증)
bash "$HANDOFF_SH" notify plan --summary "계획 완료"
bash "$HANDOFF_SH" status
# Expected: status=implementing

# 5. cleanup (테스트 데이터 정리)
bash "$HANDOFF_SH" notify merge --summary "테스트 머지"
bash "$HANDOFF_SH" status
# Expected: active (none), watermark 파일 없음

# 6. --skip-design
bash "$HANDOFF_SH" notify start --skip-design --scopes tools --summary "스킵 테스트"
bash "$HANDOFF_SH" status
# Expected: status=implementing

# 7. 정리
bash "$HANDOFF_SH" notify merge --summary "스킵 테스트 머지"
```

- [ ] **Step 8: 커밋**

```bash
git add apex_tools/branch-handoff.sh
git commit -m "feat(tools): branch-handoff 상태 머신 — notify plan, --skip-design, status 검증"
git push
```

---

### Task 2: handoff-probe.sh — Edit/Write 전면 차단

**Files:**
- Modify: `.claude/hooks/handoff-probe.sh` (전면 재작성)

**참조:** 기존 코드는 미ack 알림 감지용. 새 코드는 active 등록 + status 기반 차단.

- [ ] **Step 1: handoff-probe.sh 재작성**

핵심 로직:
```bash
#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# PreToolUse hook (Edit|Write): 핸드오프 등록 + 상태 기반 차단
#
# 1. main 브랜치 → 즉시 통과
# 2. HANDOFF_DIR 미존재 → 즉시 통과
# 3. active 미등록 → 모든 파일 차단
# 4. status별 소스/비소스 분기 차단
# 5. 미ack 알림 경고 (기존 기능 유지)

INPUT=$(cat)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BRANCH_ID=$(basename "$PROJECT_ROOT" | sed 's/apex_pipeline_//')

# main 브랜치 → 스킵
CURRENT_BRANCH=$(git -C "$PROJECT_ROOT" branch --show-current 2>/dev/null || echo "")
if [[ "$CURRENT_BRANCH" == "main" || "$CURRENT_BRANCH" == "master" ]]; then
    exit 0
fi

# HANDOFF_DIR 결정
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    HANDOFF_DIR="${LOCALAPPDATA}/apex-branch-handoff"
else
    HANDOFF_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
fi
HANDOFF_DIR="${APEX_HANDOFF_DIR:-$HANDOFF_DIR}"

# 핸드오프 미사용 환경 → 통과
[[ ! -d "$HANDOFF_DIR" ]] && exit 0

# 대상 파일 경로 추출
FILE_PATH=""
if command -v jq &>/dev/null; then
    FILE_PATH=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
else
    FILE_PATH=$(echo "$INPUT" | grep -o '"file_path"[[:space:]]*:[[:space:]]*"[^"]*"' \
        | head -1 | sed 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

# feature/bugfix 브랜치가 아니면 스킵
if [[ ! "$CURRENT_BRANCH" =~ ^(feature|bugfix)/ ]]; then
    exit 0
fi

# === 1. Active 등록 확인 ===
ACTIVE_FILE="$HANDOFF_DIR/active/${BRANCH_ID}.yml"
HANDOFF_SH="${PROJECT_ROOT}/apex_tools/branch-handoff.sh"

if [[ ! -f "$ACTIVE_FILE" ]]; then
    echo "차단: 핸드오프 미등록. 먼저 실행:" >&2
    echo "  $HANDOFF_SH notify start --scopes <s> --summary \"설명\" [--backlog <N>]" >&2
    exit 2
fi

# === 2. Status 기반 차단 ===
STATUS=$(grep '^status:' "$ACTIVE_FILE" | awk '{print $2}')
IS_SOURCE=false
if echo "$FILE_PATH" | grep -qE '\.(cpp|hpp|h|c|cc|cxx|hxx)$'; then
    IS_SOURCE=true
fi

if [[ "$IS_SOURCE" == true && "$STATUS" != "implementing" ]]; then
    case "$STATUS" in
        started)
            echo "차단: 설계 미완료(status=started). 실행:" >&2
            echo "  $HANDOFF_SH notify design --scopes <s> --summary \"설계 요약\"" >&2
            echo "  (설계 불필요 시: $HANDOFF_SH notify start --skip-design)" >&2
            ;;
        design-notified)
            echo "차단: 구현 계획 미완료(status=design-notified). 실행:" >&2
            echo "  $HANDOFF_SH notify plan --summary \"계획 요약\"" >&2
            ;;
        *)
            echo "차단: 알 수 없는 status '$STATUS'. branch-handoff.sh status로 확인." >&2
            ;;
    esac
    exit 2
fi

# === 3. 미ack 알림 경고 (기존 기능 유지) ===
[[ ! -f "$HANDOFF_DIR/index" ]] && exit 0

LAST_ID=$(tail -1 "$HANDOFF_DIR/index" 2>/dev/null | cut -d'|' -f1)
[[ -z "$LAST_ID" ]] && exit 0

WATERMARK=$(cat "$HANDOFF_DIR/watermarks/${BRANCH_ID}" 2>/dev/null || echo "")
[[ "$LAST_ID" == "$WATERMARK" ]] && exit 0

RESULT=$(bash "$HANDOFF_SH" check 2>/dev/null)

if [[ -z "$RESULT" || "$RESULT" == "No new notifications." ]]; then
    echo "$LAST_ID" > "$HANDOFF_DIR/watermarks/${BRANCH_ID}"
    exit 0
fi

echo "─── 핸드오프 알림 ───" >&2
echo "$RESULT" >&2
echo "────────────────────" >&2
echo "$LAST_ID" > "$HANDOFF_DIR/watermarks/${BRANCH_ID}"
exit 0
```

- [ ] **Step 2: 수동 검증 — 미등록 차단**

```bash
# 미등록 상태에서 Edit 시도 → 차단 확인
# (실제 hook은 Claude Code가 트리거하지만, 스크립트 직접 테스트)
echo '{"tool_input":{"file_path":"test.md"}}' | bash .claude/hooks/handoff-probe.sh
# Expected: exit 2, "차단: 핸드오프 미등록"
```

- [ ] **Step 3: 수동 검증 — started + 소스 차단**

```bash
# notify start 후 소스 파일 편집 시도
bash apex_tools/branch-handoff.sh notify start --scopes tools --summary "테스트"
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
# Expected: exit 2, "차단: 설계 미완료"

echo '{"tool_input":{"file_path":"test.md"}}' | bash .claude/hooks/handoff-probe.sh
# Expected: exit 0 (비소스 허용)
```

- [ ] **Step 4: 수동 검증 — implementing 허용**

```bash
# --skip-design으로 implementing 상태 진입
bash apex_tools/branch-handoff.sh notify merge --summary "정리"
bash apex_tools/branch-handoff.sh notify start --skip-design --scopes tools --summary "테스트"
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
# Expected: exit 0 (소스 허용)

# 정리
bash apex_tools/branch-handoff.sh notify merge --summary "정리"
```

- [ ] **Step 5: 수동 검증 — main 브랜치 스킵**

```bash
# main 브랜치에서는 모든 편집 허용 확인
# (현재 main이면 스크립트가 exit 0)
git branch --show-current  # main 확인
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
# Expected: exit 0
```

- [ ] **Step 6: 커밋**

```bash
git add .claude/hooks/handoff-probe.sh
git commit -m "feat(tools): handoff-probe 강화 — active 미등록 + status 기반 전면 차단"
git push
```

---

### Task 3: validate-handoff.sh — git commit 차단

**Files:**
- Modify: `.claude/hooks/validate-handoff.sh:27-48` (probe 섹션 뒤에 git commit 체크 삽입)

- [ ] **Step 1: git commit 시 active 등록 확인 추가**

기존 probe 섹션(=== 0)과 merge gate 섹션(=== 1) 사이에 삽입:

```bash
# === 0.5) git commit 시 active 등록 확인 ===
if echo "$COMMAND" | grep -qE '^git commit|git commit '; then
    CURRENT_BRANCH=$(git -C "$PROJECT_ROOT" branch --show-current 2>/dev/null || echo "")

    # main/master 스킵
    if [[ "$CURRENT_BRANCH" != "main" && "$CURRENT_BRANCH" != "master" ]]; then
        # feature/bugfix 브랜치만 체크
        if [[ "$CURRENT_BRANCH" =~ ^(feature|bugfix)/ ]]; then
            _ACTIVE_FILE="${_PROBE_HANDOFF_DIR}/active/${BRANCH_ID}.yml"
            if [[ -d "$_PROBE_HANDOFF_DIR" && ! -f "$_ACTIVE_FILE" ]]; then
                echo "차단: 핸드오프 미등록 상태에서 커밋 불가." >&2
                echo "  먼저 실행: $HANDOFF_SH notify start --scopes <s> --summary \"설명\"" >&2
                exit 2
            fi
        fi
    fi
fi
```

- [ ] **Step 2: 수동 검증**

```bash
# 미등록 상태에서 git commit 시도 → 차단 확인
echo '{"tool_input":{"command":"git commit -m test"}}' | bash .claude/hooks/validate-handoff.sh
# Expected: exit 2, "차단: 핸드오프 미등록 상태에서 커밋 불가"
```

- [ ] **Step 3: 커밋**

```bash
git add .claude/hooks/validate-handoff.sh
git commit -m "feat(tools): validate-handoff에 git commit 시 active 등록 확인 추가"
git push
```

---

### Task 4: session-context.sh — 경고 메시지 강화

**Files:**
- Modify: `apex_tools/session-context.sh:48-50` (경고 메시지 강화)

- [ ] **Step 1: 경고 메시지에 차단 예고 추가**

기존:
```bash
echo "WARNING: 이 워크스페이스(${BRANCH_ID})가 핸드오프 시스템에 미등록 상태!"
echo "  → 즉시 실행: branch-handoff.sh notify start --scopes <s> --summary \"설명\" [--backlog <N>]"
```

변경:
```bash
echo "WARNING [BLOCKED]: 이 워크스페이스(${BRANCH_ID})가 핸드오프 시스템에 미등록 상태!"
echo "  모든 Edit/Write/git commit이 차단됩니다."
echo "  → 즉시 실행: branch-handoff.sh notify start --scopes <s> --summary \"설명\" [--backlog <N>]"
echo "  (설계 불필요 시: branch-handoff.sh notify start --skip-design --scopes <s> --summary \"설명\")"
```

- [ ] **Step 2: 등록된 경우 status 표시 강화**

기존 `grep '^status:'` 출력에 다음 단계 안내 추가:
```bash
_status=$(grep '^status:' "${HANDOFF_DIR}/active/${BRANCH_ID}.yml" 2>/dev/null | awk '{print $2}')
echo "  status: ${_status}"
case "$_status" in
    started)
        echo "  → 다음: notify design (설계 완료 시) 또는 notify start --skip-design (설계 불필요 시)" ;;
    design-notified)
        echo "  → 다음: notify plan (구현 계획 완료 시)" ;;
    implementing)
        echo "  → 구현 진행 중 (소스 편집 허용)" ;;
esac
```

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/session-context.sh
git commit -m "feat(tools): session-context 핸드오프 경고 강화 — 차단 예고 + 다음 단계 안내"
git push
```

---

### Task 5: 통합 검증 + clang-format + 문서 갱신

**Files:**
- Verify: 4개 수정 파일 전체
- Modify: `CLAUDE.md` (핸드오프 섹션 갱신)

- [ ] **Step 1: E2E 시나리오 검증**

전체 흐름 테스트 (notify start → design → plan → 소스 편집 → commit → merge):
```bash
HANDOFF_SH="$(pwd)/apex_tools/branch-handoff.sh"

# 1. 미등록 → Edit 차단
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
echo "Exit: $?"  # Expected: 2

# 2. notify start
bash "$HANDOFF_SH" notify start --scopes tools --summary "통합 테스트"

# 3. started → 소스 차단, 비소스 허용
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
echo "Exit: $?"  # Expected: 2
echo '{"tool_input":{"file_path":"test.md"}}' | bash .claude/hooks/handoff-probe.sh
echo "Exit: $?"  # Expected: 0

# 4. notify design
bash "$HANDOFF_SH" notify design --scopes tools --summary "설계 완료"

# 5. design-notified → 소스 차단
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
echo "Exit: $?"  # Expected: 2

# 6. notify plan
bash "$HANDOFF_SH" notify plan --summary "계획 완료"

# 7. implementing → 소스 허용
echo '{"tool_input":{"file_path":"test.cpp"}}' | bash .claude/hooks/handoff-probe.sh
echo "Exit: $?"  # Expected: 0

# 8. git commit 허용
echo '{"tool_input":{"command":"git commit -m test"}}' | bash .claude/hooks/validate-handoff.sh
echo "Exit: $?"  # Expected: 0

# 9. 정리
bash "$HANDOFF_SH" notify merge --summary "통합 테스트 완료"
bash "$HANDOFF_SH" status
```

- [ ] **Step 2: CLAUDE.md 핸드오프 섹션 갱신**

`CLAUDE.md` 브랜치 인수인계 섹션에 상태 머신 + --skip-design 설명 추가.

- [ ] **Step 3: 최종 커밋**

```bash
git add CLAUDE.md
git commit -m "docs(claude): 핸드오프 상태 머신 + --skip-design 문서 갱신"
git push
```
