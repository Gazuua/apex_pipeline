#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# PreToolUse hook: 브랜치 핸드오프 규칙 강제
#   1) gh pr merge 시 gate check (미ack 알림 있으면 차단)
#   2) git checkout -b / git switch -c 시 notify start 리마인더

INPUT=$(cat)

# command 추출
if command -v jq &>/dev/null; then
    COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
else
    COMMAND=$(echo "$INPUT" | grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

[[ -z "$COMMAND" ]] && exit 0

# 프로젝트 루트 결정
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HANDOFF_SH="${PROJECT_ROOT}/apex_tools/branch-handoff.sh"

# branch-handoff.sh가 없으면 무시
[[ ! -f "$HANDOFF_SH" ]] && exit 0

# === 1) gh pr merge 시 gate check ===
if echo "$COMMAND" | grep -q 'gh pr merge'; then
    GATE_RESULT=$(bash "$HANDOFF_SH" check --gate merge 2>&1)
    GATE_EXIT=$?

    if [[ $GATE_EXIT -ne 0 ]]; then
        echo "차단: 핸드오프 gate check 실패. 미ack 알림이 있습니다." >&2
        echo "$GATE_RESULT" >&2
        echo "" >&2
        echo "해결 방법:" >&2
        echo "  1. branch-handoff.sh check  (미처리 알림 확인)" >&2
        echo "  2. branch-handoff.sh ack --id <N> --action <ACTION> --reason \"사유\"" >&2
        echo "  3. 모든 알림 ack 후 머지 재시도" >&2
        exit 2
    fi
fi

# === 2) git checkout -b / git switch -c 시 리마인더 ===
if echo "$COMMAND" | grep -qE 'git (checkout -b|switch -c)'; then
    echo "REMINDER: 새 브랜치 생성 후 반드시 핸드오프 등록을 실행하세요:" >&2
    echo "  branch-handoff.sh notify start --scopes <s> --summary \"설명\" [--backlog <N>]" >&2
    # 차단하지 않고 리마인더만 — exit 0
fi

exit 0
