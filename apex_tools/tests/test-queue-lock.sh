#!/usr/bin/env bash
set -euo pipefail

# 테스트용 임시 디렉토리 사용
TEST_DIR="$(mktemp -d)"
export APEX_BUILD_QUEUE_DIR="$TEST_DIR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QUEUE_LOCK="$SCRIPT_DIR/../queue-lock.sh"

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
assert_no_dir()      { if [[ ! -d "$1" ]]; then echo "  PASS: $2"; ((pass++)) || true; else echo "  FAIL: $2"; ((fail++)) || true; fi; }

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

# ── Test 1: 초기화 ──
echo "[Test 1] init 서브커맨드로 디렉토리 생성"
"$QUEUE_LOCK" init
assert_dir_exists "$TEST_DIR/build-queue" "build-queue 디렉토리 생성"
assert_dir_exists "$TEST_DIR/merge-queue" "merge-queue 디렉토리 생성"
assert_dir_exists "$TEST_DIR/logs" "logs 디렉토리 생성"

# ── Test 2: PID 검증 ──
echo "[Test 2] PID 생존 확인"

result=$("$QUEUE_LOCK" _check_pid $$ && echo "0" || echo "1")
assert_eq "현재 PID는 alive" "0" "$result"

result=$("$QUEUE_LOCK" _check_pid 99999 && echo "0" || echo "1")
assert_eq "존재하지 않는 PID는 dead" "1" "$result"

# ── Test 3: stale lock 감지 ──
echo "[Test 3] stale lock 감지"
mkdir -p "$TEST_DIR/build.lock"
printf "PID=99999\nBRANCH=test_branch\nACQUIRED=$(date +%s)\n" > "$TEST_DIR/build.owner"

"$QUEUE_LOCK" _detect_stale build > /dev/null 2>&1 || true
assert_no_dir "$TEST_DIR/build.lock" "stale lock 제거됨"

# ── Test 4: 큐 등록 + 순서 확인 ──
echo "[Test 4] FIFO 큐 등록 + 순서"
rm -rf "$TEST_DIR/build-queue/"*

touch "$TEST_DIR/build-queue/20260318_100000_branch_01_1111"
touch "$TEST_DIR/build-queue/20260318_100001_branch_02_2222"
touch "$TEST_DIR/build-queue/20260318_100002_branch_03_3333"

first=$(ls "$TEST_DIR/build-queue/" | sort | head -1)
assert_eq "첫 번째는 branch_01" "20260318_100000_branch_01_1111" "$first"

# ── Test 5: 고아 큐 엔트리 정리 ──
echo "[Test 5] 고아 큐 엔트리 정리"
rm -rf "$TEST_DIR/build-queue/"*

touch "$TEST_DIR/build-queue/20260318_100000_branch_01_99999"
touch "$TEST_DIR/build-queue/20260318_100001_branch_02_$$"

"$QUEUE_LOCK" _cleanup_queue build 2>/dev/null
remaining=$(ls "$TEST_DIR/build-queue/" | wc -l | tr -d ' ')
assert_eq "고아 엔트리 삭제 후 1개 남음" "1" "$remaining"

# ── Test 6: atomic lock 획득/해제 ──
echo "[Test 6] atomic lock 획득/해제"
rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner" "$TEST_DIR/build-queue/"*

"$QUEUE_LOCK" _try_lock build
assert_dir_exists "$TEST_DIR/build.lock" "lock 디렉토리 생성"
assert_file_exists "$TEST_DIR/build.owner" "owner 파일 생성"

owner_pid=$(grep '^PID=' "$TEST_DIR/build.owner" | cut -d= -f2)
assert_eq "owner PID가 기록됨" "true" "$([[ -n "$owner_pid" ]] && echo true || echo false)"

"$QUEUE_LOCK" _release_lock build
assert_no_dir "$TEST_DIR/build.lock" "lock 디렉토리 삭제"

# ── Test 7: 이중 lock 획득 방지 ──
echo "[Test 7] 이중 lock 획득 방지"
mkdir -p "$TEST_DIR/build.lock"
printf "PID=$$\nBRANCH=test\nACQUIRED=$(date +%s)\n" > "$TEST_DIR/build.owner"

result=$("$QUEUE_LOCK" _try_lock build && echo "0" || echo "1")
assert_eq "이미 잠긴 lock은 실패" "1" "$result"

rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner"

echo ""
echo "=========================================="
echo "결과: pass=$pass, fail=$fail"
echo "=========================================="
[[ $fail -eq 0 ]] && exit 0 || exit 1
