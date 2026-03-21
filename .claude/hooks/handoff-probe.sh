#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# PreToolUse hook (Edit|Write): 핸드오프 등록 + 상태 기반 차단
#
# 1. main/master 브랜치 → 즉시 통과
# 2. feature/bugfix 외 브랜치 → 즉시 통과
# 3. HANDOFF_DIR 미존재 → 즉시 통과
# 4. active 미등록 → 모든 파일 차단
# 5. status별 소스/비소스 분기 차단
# 6. 미ack 알림 경고 (기존 기능 유지)

INPUT=$(cat)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BRANCH_ID=$(basename "$PROJECT_ROOT" | sed 's/apex_pipeline_//')

# main/master 브랜치 → 스킵
CURRENT_BRANCH=$(git -C "$PROJECT_ROOT" branch --show-current 2>/dev/null || echo "")
if [[ "$CURRENT_BRANCH" == "main" || "$CURRENT_BRANCH" == "master" ]]; then
    exit 0
fi

# feature/bugfix 브랜치가 아니면 스킵
if [[ ! "$CURRENT_BRANCH" =~ ^(feature|bugfix)/ ]]; then
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

HANDOFF_SH="${PROJECT_ROOT}/apex_tools/branch-handoff.sh"

# 대상 파일 경로 추출
FILE_PATH=""
if command -v jq &>/dev/null; then
    FILE_PATH=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
else
    FILE_PATH=$(echo "$INPUT" | grep -o '"file_path"[[:space:]]*:[[:space:]]*"[^"]*"' \
        | head -1 | sed 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

# === 1. Active 등록 확인 ===
ACTIVE_FILE="$HANDOFF_DIR/active/${BRANCH_ID}.yml"

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
