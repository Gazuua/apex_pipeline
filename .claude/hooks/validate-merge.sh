#!/usr/bin/env bash
# PreToolUse hook: lock 미획득 상태에서 gh pr merge 차단
# queue-lock.sh merge acquire 후에만 머지 가능

INPUT=$(cat)

# command/cwd 추출: jq 사용 가능하면 jq, 아니면 grep+sed fallback
if command -v jq &>/dev/null; then
    COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
    CWD=$(echo "$INPUT" | jq -r '.cwd // empty')
else
    COMMAND=$(echo "$INPUT" | grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
    CWD=$(echo "$INPUT" | grep -o '"cwd"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"cwd"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

[[ -z "$COMMAND" ]] && exit 0

# gh pr merge가 아니면 무시
if ! echo "$COMMAND" | grep -q 'gh pr merge'; then
    exit 0
fi

# queue 디렉토리 결정
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    DEFAULT_QUEUE_DIR="${LOCALAPPDATA}/apex-build-queue"
else
    DEFAULT_QUEUE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-build-queue"
fi
QUEUE_DIR="${APEX_BUILD_QUEUE_DIR:-$DEFAULT_QUEUE_DIR}"

# lock 존재 확인
if [[ ! -d "$QUEUE_DIR/merge.lock" ]]; then
    echo "차단: 먼저 queue-lock.sh merge acquire를 실행하세요." >&2
    exit 2
fi

# owner 브랜치 확인
OWNER_FILE="$QUEUE_DIR/merge.owner"
if [[ -f "$OWNER_FILE" ]]; then
    OWNER_BRANCH=$(grep '^BRANCH=' "$OWNER_FILE" | cut -d= -f2)
    if [[ -n "$OWNER_BRANCH" ]] && ! echo "$CWD" | grep -qE "(^|/)${OWNER_BRANCH}(/|$)"; then
        echo "차단: merge lock 소유자가 $OWNER_BRANCH입니다 (현재: $CWD). 먼저 queue-lock.sh merge acquire를 실행하세요." >&2
        exit 2
    fi
fi

exit 0
