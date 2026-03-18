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

echo ""
echo "=========================================="
echo "결과: pass=$pass, fail=$fail"
echo "=========================================="
[[ $fail -eq 0 ]] && exit 0 || exit 1
