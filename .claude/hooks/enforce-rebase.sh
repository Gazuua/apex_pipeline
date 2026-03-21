#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#
# PreToolUse hook: git push / gh pr create 전 origin/main 기반 rebase 강제
#
# 목적: 충돌 상태 PR이 CI 트리거를 차단하고 타 PR에 영향을 주는 문제 방지.
# main/master 브랜치에서는 스킵.

INPUT=$(cat)

# command 추출
if command -v jq &>/dev/null; then
    COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
else
    COMMAND=$(echo "$INPUT" | grep -o '"command"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
fi

[[ -z "$COMMAND" ]] && exit 0

# git push 또는 gh pr create가 아니면 무시
IS_PUSH=false
IS_PR_CREATE=false
if echo "$COMMAND" | grep -qE 'git\s+push'; then
    IS_PUSH=true
elif echo "$COMMAND" | grep -qE 'gh\s+pr\s+create'; then
    IS_PR_CREATE=true
else
    exit 0
fi

# 현재 브랜치 확인 — main/master면 스킵
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
if [[ "$CURRENT_BRANCH" == "main" || "$CURRENT_BRANCH" == "master" ]]; then
    exit 0
fi

# detached HEAD면 스킵
if [[ "$CURRENT_BRANCH" == "HEAD" ]]; then
    exit 0
fi

# origin/main fetch
git fetch origin main --quiet 2>/dev/null

# origin/main 기준으로 behind 여부 확인
BEHIND=$(git rev-list --count HEAD..origin/main 2>/dev/null || echo "0")

if [[ "$BEHIND" -gt 0 ]]; then
    # rebase 필요 — 자동 실행
    if git rebase origin/main --quiet 2>/dev/null; then
        echo "[enforce-rebase] origin/main 기준 rebase 완료 (${BEHIND}개 커밋 반영)" >&2
    else
        # rebase 충돌 발생 — 에이전트에게 알리고 차단
        git rebase --abort 2>/dev/null
        echo "차단: origin/main rebase 중 충돌 발생. 수동으로 rebase 해결 후 다시 시도하세요." >&2
        echo "  git fetch origin main && git rebase origin/main" >&2
        exit 2
    fi
fi

exit 0
