#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# cleanup-branches.sh
# 워크트리 + 로컬/리모트 브랜치를 한 번에 정리하는 스크립트
#
# 사용법:
#   ./cleanup-branches.sh            # dry-run (목록만 출력)
#   ./cleanup-branches.sh --execute  # 실제 삭제 수행
#
# 안전장치:
#   - main/master 보호 (절대 삭제 금지)
#   - 머지되지 않은 브랜치는 삭제 대상에서 제외 (경고만 출력)
#   - 변경사항/untracked 파일이 있는 워크트리는 삭제 대상에서 제외
#   - dry-run이 기본값
# ============================================================================

# ── 보호 브랜치 (절대 삭제 금지) ──
PROTECTED_BRANCHES=("main" "master")

is_protected() {
    local branch="$1"
    for protected in "${PROTECTED_BRANCHES[@]}"; do
        if [[ "$branch" == "$protected" ]]; then
            return 0
        fi
    done
    return 1
}

# main에 머지되었는지 확인
#   1차: git merge-base --is-ancestor (일반 merge 커버)
#   2차: gh pr list로 해당 브랜치의 머지된 PR 존재 여부 (squash merge 커버)
#   gh CLI가 없는 환경에서는 1차 결과만 사용
is_merged_to_main() {
    local branch="$1"

    # 1차: 조상 관계 확인 (일반 merge)
    if git merge-base --is-ancestor "$branch" main 2>/dev/null; then
        return 0
    fi

    # 2차: squash merge fallback — gh CLI로 머지된 PR 확인
    if command -v gh &>/dev/null; then
        local gh_branch="${branch#origin/}"
        local merged_count
        merged_count="$(gh pr list --head "$gh_branch" --state merged --json number --jq 'length' 2>/dev/null || echo "0")"
        if [[ "$merged_count" -gt 0 ]]; then
            return 0
        fi
    fi

    # 3차: blob hash 비교 — PR 없이 squash merge된 경우
    #   브랜치가 변경한 모든 파일의 blob hash가 main과 동일하면
    #   내용이 이미 main에 반영된 것으로 판단
    local merge_base
    merge_base="$(git merge-base "$branch" origin/main 2>/dev/null || true)"
    if [[ -n "$merge_base" ]]; then
        local changed_files
        changed_files="$(git diff --name-only "$merge_base" "$branch" 2>/dev/null || true)"
        if [[ -n "$changed_files" ]]; then
            local all_match=true
            while IFS= read -r file; do
                [[ -z "$file" ]] && continue
                local branch_blob main_blob
                branch_blob="$(git rev-parse "$branch:$file" 2>/dev/null || echo "MISSING_BRANCH")"
                main_blob="$(git rev-parse "origin/main:$file" 2>/dev/null || echo "MISSING_MAIN")"
                if [[ "$branch_blob" != "$main_blob" ]]; then
                    all_match=false
                    break
                fi
            done <<< "$changed_files"
            if $all_match; then
                return 0
            fi
        fi
    fi

    return 1
}

# ── 모드 결정 ──
EXECUTE=false
if [[ "${1:-}" == "--execute" ]]; then
    EXECUTE=true
fi

# ── 리포지토리 루트로 이동 ──
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if $EXECUTE; then
    echo "=========================================="
    echo "  cleanup-branches.sh — 실행 모드"
    echo "=========================================="
else
    echo "=========================================="
    echo "  cleanup-branches.sh — dry-run 모드"
    echo "=========================================="
fi
echo ""

# ── git fetch (리모트 상태 최신화) ──
echo "리모트 정보 갱신 중..."
git fetch --prune origin 2>/dev/null || true
echo ""

# ============================================================================
# [1] 워크트리 정리
# ============================================================================
echo "[Worktrees] ================================================"

worktree_targets=()
worktree_warnings=()
# 변경사항이 있는 워크트리의 브랜치명을 저장 — 로컬 브랜치 섹션에서 참조
declare -A worktree_dirty_branches
while IFS= read -r line; do
    # git worktree list 출력: /path/to/worktree  <hash> [branch]
    wt_path="$(echo "$line" | awk '{print $1}')"
    wt_branch="$(echo "$line" | sed -n 's/.*\[\(.*\)\]/\1/p')"

    # main 워크트리(bare 포함)는 건너뜀
    if [[ -z "$wt_branch" ]] || is_protected "$wt_branch"; then
        continue
    fi

    # 머지되지 않은 브랜치는 경고만 출력
    if ! is_merged_to_main "$wt_branch"; then
        worktree_warnings+=("⚠ 미머지: $wt_path [$wt_branch]")
        continue
    fi

    # 변경사항/untracked 파일이 있으면 경고만 출력
    if [[ -d "$wt_path" ]]; then
        local_changes="$(git -C "$wt_path" status --porcelain 2>/dev/null || true)"
        if [[ -n "$local_changes" ]]; then
            worktree_warnings+=("⚠ 변경사항 있음: $wt_path [$wt_branch]")
            worktree_dirty_branches["$wt_branch"]=1
            continue
        fi
    fi

    worktree_targets+=("$wt_path|$wt_branch")
done < <(git worktree list)

if [[ ${#worktree_targets[@]} -eq 0 ]] && [[ ${#worktree_warnings[@]} -eq 0 ]]; then
    echo "  (정리할 워크트리 없음)"
else
    for entry in "${worktree_targets[@]+"${worktree_targets[@]}"}"; do
        [[ -z "$entry" ]] && continue
        wt_path="${entry%%|*}"
        wt_branch="${entry##*|}"
        if $EXECUTE; then
            echo "  삭제: $wt_path [$wt_branch]"
            git worktree remove --force "$wt_path" 2>&1 | sed 's/^/    /'
            # 잔여 디렉토리가 남아있으면 강제 삭제
            if [[ -d "$wt_path" ]]; then
                echo "    잔여 디렉토리 삭제: $wt_path"
                rm -rf "$wt_path"
            fi
        else
            echo "  대상: $wt_path [$wt_branch] (디렉토리도 삭제됩니다)"
        fi
    done
    for warning in "${worktree_warnings[@]+"${worktree_warnings[@]}"}"; do
        [[ -z "$warning" ]] && continue
        echo "  $warning"
    done
fi

# 워크트리 경로들의 부모 디렉토리가 비어있으면 삭제
if [[ ${#worktree_targets[@]} -gt 0 ]]; then
    declare -A worktree_parents
    for entry in "${worktree_targets[@]}"; do
        wt_path="${entry%%|*}"
        parent_dir="$(dirname "$wt_path")"
        worktree_parents["$parent_dir"]=1
    done
    for parent_dir in "${!worktree_parents[@]}"; do
        if [[ -d "$parent_dir" ]] && [[ -z "$(ls -A "$parent_dir" 2>/dev/null)" ]]; then
            if $EXECUTE; then
                echo "  빈 워크트리 디렉토리 삭제: $parent_dir"
                rm -rf "$parent_dir"
            else
                echo "  빈 워크트리 디렉토리도 삭제됩니다: $parent_dir"
            fi
        fi
    done
fi
echo ""

# ============================================================================
# [2] 로컬 브랜치 정리
# ============================================================================
echo "[Local Branches] ==========================================="

local_targets=()
local_warnings=()
while IFS= read -r branch; do
    # git branch 출력에서 앞의 공백, *(현재 브랜치), +(워크트리 브랜치) 접두사 제거
    branch="$(echo "$branch" | sed 's/^[*+ ]*//' | xargs)"

    # 빈 줄이나 보호 브랜치 건너뜀
    [[ -z "$branch" ]] && continue
    is_protected "$branch" && continue

    # 머지되지 않은 브랜치는 경고만 출력
    if ! is_merged_to_main "$branch"; then
        local_warnings+=("⚠ 미머지: $branch")
        continue
    fi

    # 워크트리에 변경사항이 있는 브랜치는 삭제 대상에서 제외
    if [[ -n "${worktree_dirty_branches[$branch]+x}" ]]; then
        local_warnings+=("⚠ 변경사항 있음 (워크트리): $branch")
        continue
    fi

    local_targets+=("$branch")
done < <(git branch)

if [[ ${#local_targets[@]} -eq 0 ]] && [[ ${#local_warnings[@]} -eq 0 ]]; then
    echo "  (정리할 로컬 브랜치 없음)"
else
    for branch in "${local_targets[@]+"${local_targets[@]}"}"; do
        [[ -z "$branch" ]] && continue
        if $EXECUTE; then
            echo "  삭제: $branch"
            git branch -D "$branch" 2>&1 | sed 's/^/    /'
        else
            echo "  대상: $branch"
        fi
    done
    for warning in "${local_warnings[@]+"${local_warnings[@]}"}"; do
        [[ -z "$warning" ]] && continue
        echo "  $warning"
    done
fi
echo ""

# ============================================================================
# [3] 리모트 브랜치 정리 (머지 완료된 것만)
# ============================================================================
echo "[Remote Branches] =========================================="

# stale 참조 정리
if $EXECUTE; then
    echo "  리모트 stale 참조 정리 (git remote prune origin)..."
    git remote prune origin 2>&1 | sed 's/^/    /'
    echo ""
fi

# 머지 완료된 리모트 브랜치 수집 (squash merge 포함)
remote_targets=()
remote_warnings=()
while IFS= read -r ref; do
    ref="$(echo "$ref" | xargs)"
    [[ -z "$ref" ]] && continue

    # origin/HEAD -> origin/main 같은 포인터 건너뜀
    [[ "$ref" == *"->"* ]] && continue

    # origin/ 접두사 제거해서 브랜치명 추출
    branch="${ref#origin/}"

    # 보호 브랜치 건너뜀
    is_protected "$branch" && continue

    # is_merged_to_main으로 squash merge까지 커버
    if is_merged_to_main "origin/$branch"; then
        remote_targets+=("$branch")
    else
        remote_warnings+=("⚠ 미머지: origin/$branch")
    fi
done < <(git branch -r 2>/dev/null)

if [[ ${#remote_targets[@]} -eq 0 ]] && [[ ${#remote_warnings[@]} -eq 0 ]]; then
    echo "  (정리할 리모트 브랜치 없음)"
else
    for branch in "${remote_targets[@]+"${remote_targets[@]}"}"; do
        [[ -z "$branch" ]] && continue
        if $EXECUTE; then
            echo "  삭제: origin/$branch"
            git push origin --delete "$branch" 2>&1 | sed 's/^/    /' || echo "    (이미 삭제되었거나 실패)"
        else
            echo "  대상: origin/$branch (머지 완료)"
        fi
    done
    for warning in "${remote_warnings[@]+"${remote_warnings[@]}"}"; do
        [[ -z "$warning" ]] && continue
        echo "  $warning"
    done
fi
echo ""

# ============================================================================
# 요약
# ============================================================================
echo "=========================================="
total=$(( ${#worktree_targets[@]} + ${#local_targets[@]} + ${#remote_targets[@]} ))
echo "  워크트리: ${#worktree_targets[@]}개"
echo "  로컬 브랜치: ${#local_targets[@]}개"
echo "  리모트 브랜치: ${#remote_targets[@]}개 (머지 완료)"
echo "  합계: ${total}개"

# 경고 합계
warning_total=$(( ${#worktree_warnings[@]} + ${#local_warnings[@]} + ${#remote_warnings[@]} ))
if [[ $warning_total -gt 0 ]]; then
    echo "  ──────────────────────────────"
    echo "  제외 (미머지/변경사항): ${warning_total}개"
fi
echo "=========================================="

if ! $EXECUTE; then
    echo ""
    echo "위 항목을 실제로 삭제하려면:"
    echo "  $0 --execute"
fi
