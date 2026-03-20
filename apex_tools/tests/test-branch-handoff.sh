#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

set -euo pipefail

TEST_DIR="$(mktemp -d)"
export APEX_HANDOFF_DIR="$TEST_DIR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HANDOFF="$SCRIPT_DIR/../branch-handoff.sh"

pass=0; fail=0
assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS: $desc"; ((pass++)) || true
    else
        echo "  FAIL: $desc (expected='$expected', actual='$actual')"; ((fail++)) || true
    fi
}
assert_dir_exists()  { if [[ -d "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_file_exists() { if [[ -f "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_no_file()     { if [[ ! -f "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_no_dir()      { if [[ ! -d "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }
assert_contains()    { if echo "$2" | grep -q "$1"; then echo "  PASS: $3"; ((pass++)) || true; else echo "  FAIL: $3 (pattern='$1' not in output)"; ((fail++)) || true; fi; }
assert_exit_code()   { if [[ "$1" == "$2" ]]; then echo "  PASS: $3"; ((pass++)) || true; else echo "  FAIL: $3 (expected exit=$1, got=$2)"; ((fail++)) || true; fi; }

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

# ── Test 1: init ──
echo "[Test 1] init으로 디렉토리 생성"
"$HANDOFF" init
assert_file_exists "$TEST_DIR/index" "index 파일 생성"
assert_dir_exists "$TEST_DIR/watermarks" "watermarks/ 생성"
assert_dir_exists "$TEST_DIR/payloads" "payloads/ 생성"
assert_dir_exists "$TEST_DIR/active" "active/ 생성"
assert_dir_exists "$TEST_DIR/responses" "responses/ 생성"
assert_dir_exists "$TEST_DIR/backlog-status" "backlog-status/ 생성"
assert_dir_exists "$TEST_DIR/archive" "archive/ 생성"

# ── Test 2: index lock 획득/해제 ──
echo "[Test 2] index lock 획득/해제"
"$HANDOFF" _lock_index
assert_dir_exists "$TEST_DIR/index.lock" "index.lock 디렉토리 생성"
assert_file_exists "$TEST_DIR/index.lock/owner" "owner 파일 생성"

owner_pid=$(grep '^PID=' "$TEST_DIR/index.lock/owner" | cut -d= -f2)
assert_eq "owner PID 기록됨" "true" "$([[ -n "$owner_pid" ]] && echo true || echo false)"

"$HANDOFF" _unlock_index
assert_no_dir "$TEST_DIR/index.lock" "index.lock 디렉토리 삭제"

# ── Test 3: stale index lock 감지 ──
echo "[Test 3] stale index lock 감지 (dead PID)"
mkdir -p "$TEST_DIR/index.lock"
printf "PID=99999\nACQUIRED=$(date +%s)\n" > "$TEST_DIR/index.lock/owner"
"$HANDOFF" _lock_index 2>/dev/null
assert_dir_exists "$TEST_DIR/index.lock" "stale 제거 후 재획득"
"$HANDOFF" _unlock_index

# ── Test 4: append_index 원자적 ID 생성 ──
echo "[Test 4] append_index 원자적 ID 생성"
> "$TEST_DIR/index"
result=$("$HANDOFF" _append_index "tier1" "branch_01" "core,shared" "테스트 알림 1")
assert_eq "첫 ID는 1" "1" "$result"

result=$("$HANDOFF" _append_index "tier2" "branch_02" "gateway" "테스트 알림 2")
assert_eq "두 번째 ID는 2" "2" "$result"

line_count=$(wc -l < "$TEST_DIR/index" | tr -d ' ')
assert_eq "index에 2줄" "2" "$line_count"

first_line=$(head -1 "$TEST_DIR/index")
assert_contains "1|tier1|branch_01|core,shared|" "$first_line" "첫 줄 포맷 정확"

# ── Test 5: notify start ──
echo "[Test 5] notify start — Tier 1 알림"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소"

assert_file_exists "$TEST_DIR/active/branch_03.yml" "active 파일 생성"
assert_file_exists "$TEST_DIR/backlog-status/branch_03.yml" "backlog-status 파일 생성"

index_line=$(cat "$TEST_DIR/index")
assert_contains "tier1" "$index_line" "index에 tier1 기록"
assert_contains "branch_03" "$index_line" "index에 branch_id 기록"
assert_contains "core,shared" "$index_line" "index에 scopes 기록"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "status: started" "$active_content" "active status=started"
assert_contains "backlog: 89" "$active_content" "active에 backlog ID"

backlog_content=$(cat "$TEST_DIR/backlog-status/branch_03.yml")
assert_contains "backlog: 89" "$backlog_content" "backlog-status에 ID"

# ── Test 6: notify design ──
echo "[Test 6] notify design — Tier 2 알림"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/payloads/"*

# 먼저 active 파일 생성
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소"

# payload 파일 준비
echo "## 테스트 설계 내용" > "$TEST_DIR/tmp_payload.md"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared,gateway --summary "ErrorCode 분리" \
  --payload-file "$TEST_DIR/tmp_payload.md"

assert_file_exists "$TEST_DIR/payloads/2.md" "payload 파일 생성"

payload_content=$(cat "$TEST_DIR/payloads/2.md")
assert_contains "tier: tier2" "$payload_content" "payload에 tier 기록"
assert_contains "branch: branch_03" "$payload_content" "payload에 branch 기록"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "status: designing" "$active_content" "active status→designing"
assert_contains "latest_tier2_id: 2" "$active_content" "active에 latest_tier2_id"

# ── Test 7: notify design 재발행 (SUPERSEDE) ──
echo "[Test 7] notify design 재발행 — SUPERSEDE"
echo "## 변경된 설계" > "$TEST_DIR/tmp_payload2.md"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared,gateway --summary "방향 변경" \
  --payload-file "$TEST_DIR/tmp_payload2.md"

index_last=$(tail -1 "$TEST_DIR/index")
assert_contains "SUPERSEDE:2" "$index_last" "index에 SUPERSEDE 태그"

assert_file_exists "$TEST_DIR/payloads/3.md" "새 payload 파일 생성"
payload3=$(cat "$TEST_DIR/payloads/3.md")
assert_contains "supersedes: 2" "$payload3" "payload에 supersedes 필드"

active_content=$(cat "$TEST_DIR/active/branch_03.yml")
assert_contains "latest_tier2_id: 3" "$active_content" "latest_tier2_id 갱신"

# ── Test 8: notify merge ──
echo "[Test 8] notify merge — Tier 3 알림 + 정리"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify start --backlog 99 --scopes ci --summary "Valgrind fix"
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify design --scopes ci --summary "Valgrind 필터 수정"

assert_file_exists "$TEST_DIR/active/branch_01.yml" "merge 전 active 존재"
assert_file_exists "$TEST_DIR/backlog-status/branch_01.yml" "merge 전 backlog-status 존재"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify merge --summary "BACKLOG-99 머지 완료"

assert_no_file "$TEST_DIR/active/branch_01.yml" "merge 후 active 삭제"
assert_no_file "$TEST_DIR/backlog-status/branch_01.yml" "merge 후 backlog-status 삭제"

index_last=$(tail -1 "$TEST_DIR/index")
assert_contains "tier3" "$index_last" "index에 tier3 기록"
assert_contains "branch_01" "$index_last" "index에 branch 기록"

# ── Test 9: check — 새 알림 없음 ──
echo "[Test 9] check — 새 알림 없음"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/watermarks/"*

output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" "$HANDOFF" check 2>&1)
assert_contains "No new notifications" "$output" "빈 index에서 알림 없음"

# ── Test 10: check — 새 알림 있음 ──
echo "[Test 10] check — 새 알림 발견"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/watermarks/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "테스트"

output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" "$HANDOFF" check 2>&1)
assert_contains "#1" "$output" "알림 #1 표시"
assert_contains "tier1" "$output" "tier1 표시"

# ── Test 11: check --scopes 필터링 ──
echo "[Test 11] check --scopes 필터링"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/watermarks/"* "$TEST_DIR/responses/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier1" "branch_03" "core,shared" "코어 작업" > /dev/null
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" _append_index "tier1" "branch_01" "ci" "CI 작업" > /dev/null

# gateway 스코프로 필터 → 매칭 없음
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_02" "$HANDOFF" check --scopes gateway 2>&1)
assert_contains "No new notifications" "$output" "gateway 스코프 필터 → 매칭 없음"

# core 스코프로 필터 → 1건 매칭
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_02" "$HANDOFF" check --scopes core 2>&1)
assert_contains "#1" "$output" "core 스코프 필터 → #1 매칭"

# ── Test 12: check --gate — 차단 ──
echo "[Test 12] check --gate — 미응답 시 차단"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/watermarks/"* "$TEST_DIR/responses/"* "$TEST_DIR/active/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core,shared" "ErrorCode 분리" > /dev/null

mkdir -p "$TEST_DIR/active"
printf "branch: branch_01\nstatus: designing\nscopes:\n  - core\nsummary: \"test\"\nstarted_at: \"2026-03-20 14:00:00\"\nupdated_at: \"2026-03-20 14:00:00\"\n" > "$TEST_DIR/active/branch_01.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "게이트 차단 시 exit code 1" "1" "$exit_code"
assert_contains "GATE:design" "$output" "GATE:design 출력"
assert_contains "BLOCKED" "$output" "BLOCKED 출력"

# ── Test 13: check --gate — 클리어 ──
echo "[Test 13] check --gate — ack 후 클리어"
mkdir -p "$TEST_DIR/responses/1"
echo "action: no-impact" > "$TEST_DIR/responses/1/branch_01.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "게이트 클리어 시 exit code 0" "0" "$exit_code"
assert_contains "CLEAR" "$output" "CLEAR 출력"

# ── Test 14: SUPERSEDE된 알림은 게이트에서 제외 ──
echo "[Test 14] SUPERSEDE된 알림 — 게이트 자동 dismiss"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/responses/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "원본 설계" > /dev/null
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "[SUPERSEDE:1] 변경된 설계" > /dev/null

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>&1) || exit_code=$?
assert_eq "SUPERSEDE: 1건만 블록" "1" "$exit_code"
assert_contains "#2" "$output" "#2만 표시"

# ── Test 15: ack ──
echo "[Test 15] ack — 대응 선언"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/responses/"* "$TEST_DIR/watermarks/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" _append_index "tier2" "branch_03" "core" "테스트" > /dev/null

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 1 --action no-impact --reason "영향 없음"

assert_file_exists "$TEST_DIR/responses/1/branch_01.yml" "response 파일 생성"

response=$(cat "$TEST_DIR/responses/1/branch_01.yml")
assert_contains "action: no-impact" "$response" "action 기록"
assert_contains "reason:" "$response" "reason 기록"

# ── Test 16: ack 후 check --gate 클리어 ──
echo "[Test 16] ack 후 게이트 클리어"
exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate build --scopes core 2>&1) || exit_code=$?
assert_eq "ack 후 게이트 클리어" "0" "$exit_code"

# ── Test 17: read ──
echo "[Test 17] read — payload 출력"
# Test 6에서 생성한 payloads/2.md 사용
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" read 2 2>&1) || true
assert_contains "tier: tier2" "$output" "payload 내용 출력"

# ── Test 18: backlog-check — FIXING ──
echo "[Test 18] backlog-check"
rm -f "$TEST_DIR/backlog-status/"*
mkdir -p "$TEST_DIR/backlog-status"
printf "backlog: 89\nbranch: branch_03\nstarted_at: \"2026-03-20 14:30:00\"\n" \
  > "$TEST_DIR/backlog-status/branch_03.yml"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 89 2>&1) || exit_code=$?
assert_eq "FIXING 시 exit 1" "1" "$exit_code"
assert_contains "FIXING" "$output" "FIXING 출력"
assert_contains "branch_03" "$output" "소유 브랜치 표시"

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 42 2>&1) || exit_code=$?
assert_eq "AVAILABLE 시 exit 0" "0" "$exit_code"
assert_contains "AVAILABLE" "$output" "AVAILABLE 출력"

# ── Test 19: status ──
echo "[Test 19] status — 전체 현황"
# active 파일 재생성 (이전 테스트에서 삭제되었을 수 있음)
mkdir -p "$TEST_DIR/active"
printf "branch: branch_03\nstatus: designing\nscopes:\n  - core\nsummary: \"테스트\"\nstarted_at: \"2026-03-20 14:30:00\"\nupdated_at: \"2026-03-20 14:30:00\"\n" \
  > "$TEST_DIR/active/branch_03.yml"

output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" status 2>&1)
assert_contains "branch_03" "$output" "활성 브랜치 표시"

# ── Test 20: cleanup — stale active 감지 ──
echo "[Test 20] cleanup — stale active 감지"
mkdir -p "$TEST_DIR/active"
printf "branch: branch_99\npid: 99999\nstatus: implementing\nscopes:\n  - core\nsummary: \"test\"\nstarted_at: \"2026-03-19 10:00:00\"\nupdated_at: \"2026-03-19 10:00:00\"\n" \
  > "$TEST_DIR/active/branch_99.yml"

APEX_HANDOFF_DIR="$TEST_DIR" APEX_HANDOFF_STALE_SEC=0 "$HANDOFF" cleanup 2>/dev/null
assert_no_file "$TEST_DIR/active/branch_99.yml" "stale active 제거"

# ── Test 21: cleanup — 고아 watermark 제거 ──
echo "[Test 21] cleanup — 고아 watermark 제거"
echo "5" > "$TEST_DIR/watermarks/branch_99"

APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" cleanup 2>/dev/null
assert_no_file "$TEST_DIR/watermarks/branch_99" "고아 watermark 제거"

# ── Test 22: 전체 라이프사이클 ──
echo "[Test 22] 전체 라이프사이클 — start → design → gate → ack → merge"
> "$TEST_DIR/index"
rm -rf "$TEST_DIR/active/"* "$TEST_DIR/backlog-status/"* "$TEST_DIR/responses/"* "$TEST_DIR/watermarks/"* "$TEST_DIR/payloads/"*

# branch_03이 작업 시작
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify start --backlog 89 --scopes core,shared --summary "역방향 의존 해소" 2>/dev/null

# branch_01이 게이트 체크 → 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate design --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 1에 의해 차단" "1" "$exit_code"

# branch_01이 ack
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 1 --action no-impact --reason "영향 없음" 2>/dev/null

# branch_03이 설계 발행
echo "## 설계 내용" > "$TEST_DIR/tmp_design.md"
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify design --scopes core,shared --summary "설계 확정" \
  --payload-file "$TEST_DIR/tmp_design.md" 2>/dev/null

# branch_01이 게이트 체크 → 새 Tier 2에 의해 다시 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate plan --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 2에 의해 재차단" "1" "$exit_code"

# branch_01이 Tier 2 ack
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 2 --action will-rebase --reason "다음 rebase에 반영" 2>/dev/null

# branch_03이 머지
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_03" \
  "$HANDOFF" notify merge --summary "BACKLOG-89 완료" 2>/dev/null

# Tier 3에 의해 다시 차단
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate build --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: Tier 3에 의해 차단" "1" "$exit_code"

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" ack --id 3 --action rebased --reason "rebase 완료" 2>/dev/null

# 최종 게이트 클리어
exit_code=0
APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" check --gate merge --scopes core 2>/dev/null || exit_code=$?
assert_eq "라이프사이클: 전부 ack 후 클리어" "0" "$exit_code"

# 정리 확인
assert_no_file "$TEST_DIR/active/branch_03.yml" "머지 후 active 삭제됨"
assert_no_file "$TEST_DIR/backlog-status/branch_03.yml" "머지 후 backlog-status 삭제됨"

# ── Test 23: backlog 중복 착수 방지 ──
echo "[Test 23] backlog 중복 착수 방지"
> "$TEST_DIR/index"
rm -f "$TEST_DIR/backlog-status/"*

APEX_HANDOFF_DIR="$TEST_DIR" BRANCH_ID="branch_01" \
  "$HANDOFF" notify start --backlog 42 --scopes gateway --summary "테스트" 2>/dev/null

exit_code=0
output=$(APEX_HANDOFF_DIR="$TEST_DIR" "$HANDOFF" backlog-check 42 2>&1) || exit_code=$?
assert_eq "이미 착수된 백로그 → exit 1" "1" "$exit_code"
assert_contains "FIXING" "$output" "FIXING 표시"

echo ""
echo "=========================================="
echo "결과: pass=$pass, fail=$fail"
echo "=========================================="
[[ $fail -eq 0 ]] && exit 0 || exit 1
