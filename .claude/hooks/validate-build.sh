#!/usr/bin/env bash
# PreToolUse hook: 빌드/벤치마크 도구 직접 호출 차단
# queue-lock.sh build|benchmark를 통해서만 실행 가능

INPUT=$(cat)

# command 추출: jq 사용 가능하면 jq, 아니면 grep+sed fallback
if command -v jq &>/dev/null; then
    COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
else
    COMMAND=$(echo "$INPUT" | grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

[[ -z "$COMMAND" ]] && exit 0

# queue-lock.sh를 통한 호출은 허용
if echo "$COMMAND" | grep -q 'queue-lock\.sh'; then
    exit 0
fi

# 읽기 전용 명령은 허용 (cat, head, grep 등으로 build.bat 내용 확인)
if echo "$COMMAND" | grep -qE '^\s*(cat|head|tail|grep|less|more|type|echo|ls|dir|file|wc|read)\b'; then
    exit 0
fi

# 차단 대상 패턴
BLOCKED_PATTERNS=(
    'cmake --build'
    'cmake --preset'
    '\bninja\b'
    '\bmsbuild\b'
    '\bcl\.exe\b'
    '(^|[;&|]\s*)build\.bat'
    'cmd\.exe.*build\.bat'
    '\bbench_\w+'
)

for pattern in "${BLOCKED_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -qE "$pattern"; then
        echo "차단: 빌드/벤치마크는 queue-lock.sh build|benchmark를 통해서만 실행할 수 있습니다. (matched: $pattern)" >&2
        exit 2
    fi
done

exit 0
