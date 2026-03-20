#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# PreToolUse hook (Edit/Write): 핸드오프 알림 경량 probe
# index watermark 비교로 변경 감지 — 변경 없으면 즉시 종료 (~1ms)
# 소스 파일(.cpp/.hpp/.h) 편집 시 미ack 알림이 있으면 차단 (exit 2)

INPUT=$(cat)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BRANCH_ID=$(basename "$PROJECT_ROOT" | sed 's/apex_pipeline_//')

if [[ -n "${LOCALAPPDATA:-}" ]]; then
    HANDOFF_DIR="${LOCALAPPDATA}/apex-branch-handoff"
else
    HANDOFF_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
fi
HANDOFF_DIR="${APEX_HANDOFF_DIR:-$HANDOFF_DIR}"

# index 파일 없으면 핸드오프 미사용 환경 — 즉시 통과
[[ ! -f "$HANDOFF_DIR/index" ]] && exit 0

# probe: 마지막 ID와 watermark 비교
LAST_ID=$(tail -1 "$HANDOFF_DIR/index" 2>/dev/null | cut -d'|' -f1)
[[ -z "$LAST_ID" ]] && exit 0

WATERMARK=$(cat "$HANDOFF_DIR/watermarks/${BRANCH_ID}" 2>/dev/null || echo "")
[[ "$LAST_ID" == "$WATERMARK" ]] && exit 0

# 변경 감지 — full check 실행
RESULT=$(bash "$PROJECT_ROOT/apex_tools/branch-handoff.sh" check 2>/dev/null)

[[ -z "$RESULT" || "$RESULT" == "No new notifications." ]] && {
    echo "$LAST_ID" > "$HANDOFF_DIR/watermarks/${BRANCH_ID}"
    exit 0
}

# 미ack 알림 존재 — 파일 확장자로 차단 여부 결정
FILE_PATH=""
if command -v jq &>/dev/null; then
    FILE_PATH=$(echo "$INPUT" | jq -r '.tool_input.file_path // empty')
else
    FILE_PATH=$(echo "$INPUT" | grep -o '"file_path"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

if echo "$FILE_PATH" | grep -qE '\.(cpp|hpp|h|c|cc|cxx|hxx)$'; then
    # 소스 파일 — 차단 (watermark 미갱신 → ack할 때까지 반복 차단)
    echo "차단: 미ack 핸드오프 알림이 있어 소스 파일 편집이 차단됩니다." >&2
    echo "" >&2
    echo "$RESULT" >&2
    echo "" >&2
    echo "해결: branch-handoff.sh ack --id <N> --action <ACTION> --reason \"사유\"" >&2
    exit 2
fi

# 비소스 파일 — 경고만 (watermark 갱신)
echo "─── 핸드오프 알림 ───" >&2
echo "$RESULT" >&2
echo "────────────────────" >&2
echo "$LAST_ID" > "$HANDOFF_DIR/watermarks/${BRANCH_ID}"
exit 0
