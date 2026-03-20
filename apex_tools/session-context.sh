#!/bin/bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# 세션 시작 시 프로젝트 컨텍스트를 stdout으로 출력
# SessionStart 훅에서 호출 — Claude가 자동으로 프로젝트 상황을 파악하게 함

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== Project Context (auto-injected) ==="

# Git 상태 요약
echo ""
echo "--- Git Status ---"
cd "${WORKSPACE}"
echo "Branch: $(git branch --show-current 2>/dev/null)"
echo ""
git status --short 2>/dev/null
echo ""
echo "Recent commits:"
git log --oneline -5 2>/dev/null

echo ""
echo "=== End Project Context ==="
