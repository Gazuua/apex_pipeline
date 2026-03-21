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
CURRENT_BRANCH=$(git branch --show-current 2>/dev/null)
echo "Branch: ${CURRENT_BRANCH}"

# main 브랜치면 자동 최신화
if [[ "$CURRENT_BRANCH" == "main" ]]; then
    echo ""
    echo "Auto-fetching latest main..."
    git fetch origin main 2>&1 | sed 's/^/  /'
    git pull origin main 2>&1 | sed 's/^/  /'
fi

echo ""
git status --short 2>/dev/null
echo ""
echo "Recent commits:"
git log --oneline -5 2>/dev/null

echo ""

# 브랜치 핸드오프 상태
if [[ -n "$CURRENT_BRANCH" && "$CURRENT_BRANCH" != "main" ]]; then
    BRANCH_ID=$(basename "${WORKSPACE}" | sed 's/apex_pipeline_//')
    if [[ -n "${LOCALAPPDATA:-}" ]]; then
        HANDOFF_DIR="${LOCALAPPDATA}/apex-branch-handoff"
    else
        HANDOFF_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
    fi
    HANDOFF_DIR="${APEX_HANDOFF_DIR:-$HANDOFF_DIR}"

    echo "--- Branch Handoff ---"

    # 미등록 경고
    if [[ ! -f "${HANDOFF_DIR}/active/${BRANCH_ID}.yml" ]]; then
        echo "WARNING: 이 워크스페이스(${BRANCH_ID})가 핸드오프 시스템에 미등록 상태!"
        echo "  → 즉시 실행: branch-handoff.sh notify start --scopes <s> --summary \"설명\" [--backlog <N>]"
    else
        echo "Registered: ${BRANCH_ID}"
        grep '^backlog:' "${HANDOFF_DIR}/active/${BRANCH_ID}.yml" 2>/dev/null | sed 's/^/  /'
        grep '^status:' "${HANDOFF_DIR}/active/${BRANCH_ID}.yml" 2>/dev/null | sed 's/^/  /'
    fi

    # 미처리 알림 확인
    PENDING=$(bash "${WORKSPACE}/apex_tools/branch-handoff.sh" check 2>/dev/null)
    if [[ -n "$PENDING" && "$PENDING" != "No new notifications." ]]; then
        echo ""
        echo "Pending notifications:"
        echo "$PENDING"
        echo "  → ack 필요: branch-handoff.sh ack --id <N> --action <ACTION> --reason \"사유\""
    fi
fi

# 활성 백로그 현황 (다른 브랜치에서 진행 중인 항목)
HANDOFF_DIR_CTX=""
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    HANDOFF_DIR_CTX="${LOCALAPPDATA}/apex-branch-handoff"
else
    HANDOFF_DIR_CTX="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
fi
HANDOFF_DIR_CTX="${APEX_HANDOFF_DIR:-$HANDOFF_DIR_CTX}"

if [[ -d "${HANDOFF_DIR_CTX}/backlog-status" ]]; then
    _active_items=""
    _prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob
    for f in "${HANDOFF_DIR_CTX}/backlog-status/"*.yml; do
        _bl_id=$(grep '^backlog:' "$f" 2>/dev/null | awk '{print $2}' || echo "")
        _bl_branch=$(grep '^branch:' "$f" 2>/dev/null | awk '{print $2}' || echo "")
        [[ -n "$_bl_id" ]] && _active_items="${_active_items}  BACKLOG-${_bl_id} → ${_bl_branch}\n"
    done
    eval "$_prev_nullglob" 2>/dev/null || true

    if [[ -n "$_active_items" ]]; then
        echo "--- Active Backlogs (다른 브랜치 진행 중) ---"
        echo -e "$_active_items"
    fi
fi

echo "=== End Project Context ==="
