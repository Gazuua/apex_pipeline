#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# PreToolUse hook (Edit/Write): 핸드오프 알림 경량 probe
# index watermark 비교로 변경 감지 — 변경 없으면 즉시 종료 (~1ms)

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
echo "$LAST_ID" > "$HANDOFF_DIR/watermarks/${BRANCH_ID}"

[[ -z "$RESULT" || "$RESULT" == "No new notifications." ]] && exit 0

echo "─── 핸드오프 알림 ───" >&2
echo "$RESULT" >&2
echo "────────────────────" >&2
exit 0
