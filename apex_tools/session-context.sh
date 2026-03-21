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
        echo "WARNING [BLOCKED]: 이 워크스페이스(${BRANCH_ID})가 핸드오프 시스템에 미등록 상태!"
        echo "  모든 Edit/Write/git commit이 차단됩니다."
        echo "  → 즉시 실행: branch-handoff.sh notify start --scopes <s> --summary \"설명\" [--backlog <N>]"
        echo "  (설계 불필요 시: branch-handoff.sh notify start --skip-design --scopes <s> --summary \"설명\")"
    else
        echo "Registered: ${BRANCH_ID}"
        grep '^backlog:' "${HANDOFF_DIR}/active/${BRANCH_ID}.yml" 2>/dev/null | sed 's/^/  /'
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

# 다른 브랜치 활성 작업 현황
# HANDOFF_DIR은 위 핸드오프 블록에서 이미 설정됨. main일 때는 여기서 설정.
if [[ -z "${HANDOFF_DIR:-}" ]]; then
    if [[ -n "${LOCALAPPDATA:-}" ]]; then
        HANDOFF_DIR="${LOCALAPPDATA}/apex-branch-handoff"
    else
        HANDOFF_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-branch-handoff"
    fi
    HANDOFF_DIR="${APEX_HANDOFF_DIR:-$HANDOFF_DIR}"
fi

_MY_BRANCH_ID=$(basename "${WORKSPACE}" | sed 's/apex_pipeline_//')

echo "--- Handoff Storage ---"
echo "Path: ${HANDOFF_DIR}"

if [[ -d "${HANDOFF_DIR}/active" ]]; then
    _active_lines=""
    _prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob
    for f in "${HANDOFF_DIR}/active/"*.yml; do
        _br=$(grep '^branch:' "$f" 2>/dev/null | awk '{print $2}' || echo "")
        # 자기 자신은 스킵
        [[ "$_br" == "$_MY_BRANCH_ID" ]] && continue

        _status=$(grep '^status:' "$f" 2>/dev/null | awk '{print $2}' || echo "")
        _scopes=$(grep '^  - ' "$f" 2>/dev/null | sed 's/  - //' | paste -sd',' - || echo "")
        _summary=$(grep '^summary:' "$f" 2>/dev/null | sed 's/summary: *"//' | sed 's/"$//' || echo "")
        _backlog=$(grep '^backlog:' "$f" 2>/dev/null | awk '{print $2}' || echo "")

        _bl_label=""
        [[ -n "$_backlog" ]] && _bl_label="BACKLOG-${_backlog} "

        if [[ -n "$_scopes" ]]; then
            _active_lines="${_active_lines}  ${_br} [${_scopes}] ${_bl_label}— ${_summary}\n"
        else
            _active_lines="${_active_lines}  ${_br} [착수 단계] ${_bl_label}— 설계 알림 수신 시 자동 통보\n"
        fi
    done
    eval "$_prev_nullglob" 2>/dev/null || true

    if [[ -n "$_active_lines" ]]; then
        echo "--- Other Active Branches ---"
        echo -e "$_active_lines"
    fi
fi

echo "=== End Project Context ==="
