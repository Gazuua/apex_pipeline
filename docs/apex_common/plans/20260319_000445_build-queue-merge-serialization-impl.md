# #55 빌드 큐잉 + 머지 직렬화 시스템 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 물리적으로 분리된 복수 브랜치 디렉토리가 동일 PC 자원을 공유할 때 빌드/머지 경합을 FIFO 큐로 직렬화하는 시스템 구축

**Architecture:** `queue-lock.sh` 단일 bash 스크립트가 FIFO 큐 + mkdir atomic lock으로 빌드/머지를 직렬화. PreToolUse hook이 우회 시도를 원천 차단. `build.bat`은 순수 빌드만 담당.

**Tech Stack:** Bash (큐/lock), Windows Batch (빌드), Claude Code PreToolUse hooks (강제), jq (hook 입력 파싱)

**Spec:** `docs/apex_common/plans/20260318_233850_build-queue-merge-serialization-design.md`

---

## 파일 구조

| 파일 | 작업 | 책임 |
|------|------|------|
| `apex_tools/queue-lock.sh` | 신규 | 통합 큐/lock 관리 — FIFO 큐, atomic lock, stale 감지, PID 검증, 채널 라우팅 |
| `apex_tools/tests/test-queue-lock.sh` | 신규 | queue-lock.sh 기능 검증 테스트 스크립트 |
| `build.bat` | 수정 | 추가 인자 전달 (--target 등). lock 로직 없음 |
| `.claude/hooks/validate-build.sh` | 신규 | PreToolUse hook — 빌드 도구 직접 호출 차단 |
| `.claude/hooks/validate-merge.sh` | 신규 | PreToolUse hook — lock 미획득 상태에서 `gh pr merge` 차단 |
| `.claude/settings.json` | 수정 | PreToolUse hook 등록 |
| `CLAUDE.md` | 수정 | 빌드/머지 명령 규칙 갱신 |

---

## Task 1: queue-lock.sh 스켈레톤 + 설정/초기화

`queue-lock.sh`의 기본 구조를 만든다. 환경변수 설정, 프로젝트 루트 감지, 큐 디렉토리 초기화, 채널 라우팅.

**Files:**
- Create: `apex_tools/queue-lock.sh`
- Create: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: 테스트 스크립트 스켈레톤 작성**

```bash
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
        echo "  PASS: $desc"; ((pass++))
    else
        echo "  FAIL: $desc (expected='$expected', actual='$actual')"; ((fail++))
    fi
}
assert_dir_exists()  { [[ -d "$1" ]] && echo "  PASS: $2" && ((pass++)) || { echo "  FAIL: $2"; ((fail++)); }; }
assert_file_exists() { [[ -f "$1" ]] && echo "  PASS: $2" && ((pass++)) || { echo "  FAIL: $2"; ((fail++)); }; }
assert_no_dir()      { [[ ! -d "$1" ]] && echo "  PASS: $2" && ((pass++)) || { echo "  FAIL: $2"; ((fail++)); }; }

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

# ── Test 1: 초기화 ──
echo "[Test 1] init 서브커맨드로 디렉토리 생성"
"$QUEUE_LOCK" init
assert_dir_exists "$TEST_DIR/build-queue" "build-queue 디렉토리 생성"
assert_dir_exists "$TEST_DIR/merge-queue" "merge-queue 디렉토리 생성"
assert_dir_exists "$TEST_DIR/logs" "logs 디렉토리 생성"

echo ""
echo "결과: pass=$pass, fail=$fail"
[[ $fail -eq 0 ]] && exit 0 || exit 1
```

- [ ] **Step 2: 테스트 실행하여 실패 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: FAIL (queue-lock.sh 미존재)

- [ ] **Step 3: queue-lock.sh 스켈레톤 구현**

```bash
#!/usr/bin/env bash
set -euo pipefail

# === Configuration ===
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BRANCH_ID="$(basename "$PROJECT_ROOT")"

# Windows: LOCALAPPDATA, Linux/Mac: XDG_DATA_HOME or ~/.local/share
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    DEFAULT_QUEUE_DIR="${LOCALAPPDATA}/apex-build-queue"
else
    DEFAULT_QUEUE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/apex-build-queue"
fi

QUEUE_DIR="${APEX_BUILD_QUEUE_DIR:-$DEFAULT_QUEUE_DIR}"
STALE_TIMEOUT="${APEX_STALE_TIMEOUT_SEC:-3600}"
POLL_INTERVAL="${APEX_POLL_INTERVAL_SEC:-1}"

# === Init ===
do_init() {
    mkdir -p "$QUEUE_DIR/build-queue"
    mkdir -p "$QUEUE_DIR/merge-queue"
    mkdir -p "$QUEUE_DIR/logs"
}

# === Main ===
main() {
    local channel="${1:-help}"
    shift || true

    case "$channel" in
        init)   do_init ;;
        build)  do_init; echo "build channel: not yet implemented" ;;
        merge)  do_init; echo "merge channel: not yet implemented" ;;
        *)      echo "Usage: queue-lock.sh <init|build|merge> [args...]"; exit 1 ;;
    esac
}

main "$@"
```

- [ ] **Step 4: 테스트 실행하여 통과 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: PASS (3/3)

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/queue-lock.sh apex_tools/tests/test-queue-lock.sh
git commit -m "feat(tools): queue-lock.sh 스켈레톤 + 테스트 인프라"
```

---

## Task 2: PID 검증 + stale lock 감지

Windows/Linux 호환 PID 검증과 stale lock 감지 로직을 구현한다.

**Files:**
- Modify: `apex_tools/queue-lock.sh`
- Modify: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: PID 검증 + stale 감지 테스트 추가**

`test-queue-lock.sh`에 추가:

```bash
# ── Test 2: PID 검증 ──
echo "[Test 2] PID 생존 확인"
"$QUEUE_LOCK" init

# 현재 프로세스의 PID는 살아있어야 함 — 내부 서브커맨드로 테스트
result=$(APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _check_pid $$; echo $?)
assert_eq "현재 PID는 alive" "0" "$result"

# 존재하지 않는 PID (99999)
result=$(APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _check_pid 99999; echo $?)
assert_eq "존재하지 않는 PID는 dead" "1" "$result"

# ── Test 3: stale lock 감지 ──
echo "[Test 3] stale lock 감지"
# 죽은 PID의 lock 생성
mkdir -p "$TEST_DIR/build.lock"
echo -e "PID=99999\nBRANCH=test_branch\nACQUIRED=$(date +%s)" > "$TEST_DIR/build.owner"

result=$(APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _detect_stale build; echo $?)
assert_eq "죽은 PID → stale 감지" "0" "$result"
assert_no_dir "$TEST_DIR/build.lock" "stale lock 제거됨"
```

- [ ] **Step 2: 테스트 실행하여 실패 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: FAIL (check_pid_alive, _detect_stale 미구현)

- [ ] **Step 3: PID 검증 함수 구현**

`queue-lock.sh`에 추가:

```bash
# === PID Check ===
check_pid_alive() {
    local pid="$1"
    if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
        # Windows: tasklist로 네이티브 프로세스 확인
        tasklist /FI "PID eq $pid" 2>/dev/null | grep -q "$pid"
    else
        # Linux/Mac: kill -0
        kill -0 "$pid" 2>/dev/null
    fi
}
```

- [ ] **Step 4: stale lock 감지 함수 구현**

`queue-lock.sh`에 추가:

```bash
# === Stale Detection ===
detect_stale_lock() {
    local channel="$1"
    local lock_dir="$QUEUE_DIR/${channel}.lock"
    local owner_file="$QUEUE_DIR/${channel}.owner"

    [[ ! -d "$lock_dir" ]] && return 1  # lock 없음 — stale 아님

    if [[ ! -f "$owner_file" ]]; then
        # owner 파일 없이 lock만 존재 — stale
        echo "[queue-lock] WARNING: orphan lock detected (no owner), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        return 0
    fi

    local pid branch acquired
    pid=$(grep '^PID=' "$owner_file" | cut -d= -f2)
    acquired=$(grep '^ACQUIRED=' "$owner_file" | cut -d= -f2)

    # 1차: PID 생존 확인
    if ! check_pid_alive "$pid"; then
        echo "[queue-lock] WARNING: stale lock (PID $pid dead), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        rm -f "$owner_file"
        return 0
    fi

    # 2차: 타임스탬프 만료
    local now elapsed
    now=$(date +%s)
    elapsed=$((now - acquired))
    if [[ $elapsed -ge $STALE_TIMEOUT ]]; then
        echo "[queue-lock] WARNING: stale lock (${elapsed}s elapsed > ${STALE_TIMEOUT}s timeout), removing"
        rmdir "$lock_dir" 2>/dev/null || rm -rf "$lock_dir"
        rm -f "$owner_file"
        return 0
    fi

    return 1  # lock은 유효
}
```

main의 case에 내부 서브커맨드 추가 (테스트용):

```bash
_check_pid)    check_pid_alive "$1" ;;
_detect_stale) detect_stale_lock "$1" ;;
```

- [ ] **Step 5: 테스트 실행하여 통과 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: PASS (전체)

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/queue-lock.sh apex_tools/tests/test-queue-lock.sh
git commit -m "feat(tools): PID 검증 + stale lock 감지 구현"
```

---

## Task 3: FIFO 큐 관리

큐 등록, 순서 확인, 고아 엔트리 정리 로직을 구현한다.

**Files:**
- Modify: `apex_tools/queue-lock.sh`
- Modify: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: FIFO 큐 테스트 추가**

`test-queue-lock.sh`에 추가:

```bash
# ── Test 4: 큐 등록 + 순서 확인 ──
echo "[Test 4] FIFO 큐 등록 + 순서"
rm -rf "$TEST_DIR/build-queue/"*

# 3개 큐 엔트리 생성 (시간순)
touch "$TEST_DIR/build-queue/20260318_100000_branch_01_1111"
touch "$TEST_DIR/build-queue/20260318_100001_branch_02_2222"
touch "$TEST_DIR/build-queue/20260318_100002_branch_03_3333"

# branch_01이 첫 번째여야 함
first=$(ls "$TEST_DIR/build-queue/" | sort | head -1)
assert_eq "첫 번째는 branch_01" "20260318_100000_branch_01_1111" "$first"

# ── Test 5: 고아 큐 엔트리 정리 ──
echo "[Test 5] 고아 큐 엔트리 정리"
rm -rf "$TEST_DIR/build-queue/"*

# 죽은 PID로 큐 엔트리 생성
touch "$TEST_DIR/build-queue/20260318_100000_branch_01_99999"
# 살아있는 PID로 큐 엔트리 생성
touch "$TEST_DIR/build-queue/20260318_100001_branch_02_$$"

APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _cleanup_queue build
remaining=$(ls "$TEST_DIR/build-queue/" | wc -l | tr -d ' ')
assert_eq "고아 엔트리 삭제 후 1개 남음" "1" "$remaining"
```

- [ ] **Step 2: 테스트 실행하여 실패 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: FAIL (_cleanup_queue 미구현)

- [ ] **Step 3: 큐 관리 함수 구현**

`queue-lock.sh`에 추가:

```bash
# === Queue Management ===
register_queue() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S)
    MY_QUEUE_FILE="$queue_dir/${timestamp}_${BRANCH_ID}_$$"
    touch "$MY_QUEUE_FILE"
}

get_queue_position() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    local first
    first=$(ls "$queue_dir" 2>/dev/null | sort | head -1)

    # 빈 큐 방어 (race condition)
    [[ -z "$first" ]] && return 1

    if [[ "$(basename "$MY_QUEUE_FILE")" == "$first" ]]; then
        return 0  # 내 차례
    else
        return 1  # 아직 대기
    fi
}

get_queue_depth() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"
    ls "$queue_dir" 2>/dev/null | wc -l | tr -d ' '
}

cleanup_orphan_queue() {
    local channel="$1"
    local queue_dir="$QUEUE_DIR/${channel}-queue"

    # nullglob: 빈 디렉토리에서 glob이 리터럴 '*'로 확장되는 것 방지
    local prev_nullglob=$(shopt -p nullglob 2>/dev/null || true)
    shopt -s nullglob

    for entry in "$queue_dir"/*; do
        [[ ! -f "$entry" ]] && continue
        local filename
        filename=$(basename "$entry")
        # 파일명에서 PID 추출 (마지막 _ 뒤)
        local pid
        pid=${filename##*_}
        if ! check_pid_alive "$pid"; then
            echo "[queue-lock] orphan queue entry removed: $filename (PID $pid dead)"
            rm -f "$entry"
        fi
    done

    # nullglob 복원
    eval "$prev_nullglob" 2>/dev/null || true
}
```

main의 case에 추가:

```bash
_cleanup_queue) cleanup_orphan_queue "$1" ;;
```

- [ ] **Step 4: 테스트 실행하여 통과 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: PASS (전체)

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/queue-lock.sh apex_tools/tests/test-queue-lock.sh
git commit -m "feat(tools): FIFO 큐 등록/순서/고아 정리 구현"
```

---

## Task 4: atomic lock 획득/해제 + 폴링 루프

mkdir atomic lock과 폴링 대기 루프를 구현한다.

**Files:**
- Modify: `apex_tools/queue-lock.sh`
- Modify: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: lock 획득/해제 테스트 추가**

`test-queue-lock.sh`에 추가:

```bash
# ── Test 6: atomic lock 획득 ──
echo "[Test 6] atomic lock 획득/해제"
rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner"

APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _acquire_lock build
assert_dir_exists "$TEST_DIR/build.lock" "lock 디렉토리 생성"
assert_file_exists "$TEST_DIR/build.owner" "owner 파일 생성"

# owner 파일에 PID 기록 확인
owner_pid=$(grep '^PID=' "$TEST_DIR/build.owner" | cut -d= -f2)
assert_eq "owner PID가 기록됨" "true" "$([[ -n "$owner_pid" ]] && echo true || echo false)"

# 해제
APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _release_lock build
assert_no_dir "$TEST_DIR/build.lock" "lock 디렉토리 삭제"

# ── Test 7: 이중 lock 획득 방지 ──
echo "[Test 7] 이중 lock 획득 방지"
mkdir -p "$TEST_DIR/build.lock"
echo "PID=$$" > "$TEST_DIR/build.owner"
echo "ACQUIRED=$(date +%s)" >> "$TEST_DIR/build.owner"

result=$(APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _try_lock build; echo $?)
assert_eq "이미 잠긴 lock은 실패" "1" "$result"

rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner"
```

- [ ] **Step 2: 테스트 실행하여 실패 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: FAIL (_acquire_lock, _release_lock, _try_lock 미구현)

- [ ] **Step 3: lock 획득/해제 함수 구현**

`queue-lock.sh`에 추가:

```bash
# === Lock Acquisition ===
try_lock() {
    local channel="$1"
    local lock_dir="$QUEUE_DIR/${channel}.lock"

    if mkdir "$lock_dir" 2>/dev/null; then
        # 성공 — owner 파일 기록
        local owner_file="$QUEUE_DIR/${channel}.owner"
        {
            echo "PID=$$"
            echo "BRANCH=$BRANCH_ID"
            echo "ACQUIRED=$(date +%s)"
            echo "STATUS=ACTIVE"
        } > "$owner_file"
        return 0
    fi
    return 1
}

release_lock() {
    local channel="$1"
    rmdir "$QUEUE_DIR/${channel}.lock" 2>/dev/null || rm -rf "$QUEUE_DIR/${channel}.lock"
    rm -f "$QUEUE_DIR/${channel}.owner"
}

acquire_lock() {
    local channel="$1"
    register_queue "$channel"

    trap 'release_lock "'"$channel"'"; rm -f "$MY_QUEUE_FILE"' EXIT

    while true; do
        cleanup_orphan_queue "$channel"
        detect_stale_lock "$channel" || true

        if get_queue_position "$channel" && try_lock "$channel"; then
            rm -f "$MY_QUEUE_FILE"
            echo "[queue-lock] lock acquired: $channel (branch=$BRANCH_ID, pid=$$)"
            return 0
        fi

        local depth owner_info=""
        depth=$(get_queue_depth "$channel")
        if [[ -f "$QUEUE_DIR/${channel}.owner" ]]; then
            local owner_branch owner_pid
            owner_branch=$(grep '^BRANCH=' "$QUEUE_DIR/${channel}.owner" | cut -d= -f2)
            owner_pid=$(grep '^PID=' "$QUEUE_DIR/${channel}.owner" | cut -d= -f2)
            owner_info=" (current: $owner_branch, PID $owner_pid)"
        fi
        echo "[queue-lock] waiting for $channel lock... queue depth: $depth$owner_info"
        sleep "$POLL_INTERVAL"
    done
}
```

main의 case에 추가:

```bash
_try_lock)     try_lock "$1" ;;
_acquire_lock) acquire_lock "$1" ;;
_release_lock) release_lock "$1" ;;
```

- [ ] **Step 4: 테스트 실행하여 통과 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: PASS (전체)

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/queue-lock.sh apex_tools/tests/test-queue-lock.sh
git commit -m "feat(tools): atomic lock 획득/해제 + 폴링 루프 구현"
```

---

## Task 5: build 채널 구현

run-and-release 패턴의 build 채널을 구현한다. lock 획득 → build.bat 실행 → lock 해제.

**Files:**
- Modify: `apex_tools/queue-lock.sh`
- Modify: `build.bat`

- [ ] **Step 1: build.bat 추가 인자 전달 지원**

`build.bat`의 cmake build 라인을 수정:

현재 `build.bat` line 4에서 `set PRESET=%~1`로 첫 번째 인자를 사용. line 26에서 `cmake --build`.

**수정 위치: line 5 (PRESET 설정 직후)에 추가 인자 파싱 삽입, line 26에서 EXTRA_ARGS 사용:**

```batch
:: line 5 이후 삽입 (PRESET 설정 직후):
set "EXTRA_ARGS="
:parse_extra
if "%~2"=="" goto :done_extra
set "EXTRA_ARGS=%EXTRA_ARGS% %~2"
shift
goto :parse_extra
:done_extra

:: line 26 변경:
:: 기존: cmake --build "build/Windows/%PRESET%"
:: 변경:
cmake --build "build/Windows/%PRESET%" %EXTRA_ARGS%
```

`%~1`은 PRESET, `%~2`부터가 추가 인자 (`--target` 등). shift 루프로 인자 수 제한 없음.

- [ ] **Step 2: build 채널 구현**

`queue-lock.sh`의 `do_build()` 함수:

```bash
do_build() {
    local preset="${1:-debug}"
    shift || true
    local extra_args=("$@")

    echo "[queue-lock] build requested: preset=$preset, args=${extra_args[*]:-none}, branch=$BRANCH_ID"

    acquire_lock "build"

    local log_file="$QUEUE_DIR/logs/${BRANCH_ID}.log"
    local -a build_cmd=(cmd.exe //c "${PROJECT_ROOT}\\build.bat" "$preset" "${extra_args[@]}")

    echo "[queue-lock] starting build: ${build_cmd[*]}"
    echo "[queue-lock] log: $log_file"

    local exit_code=0
    "${build_cmd[@]}" 2>&1 | tee "$log_file" || exit_code=$?

    if [[ $exit_code -eq 0 ]]; then
        echo "[queue-lock] build completed successfully"
    else
        echo "[queue-lock] build FAILED (exit code: $exit_code)"
    fi

    # trap EXIT가 release_lock 호출
    exit $exit_code
}
```

- [ ] **Step 3: 수동 검증 — 빌드 정상 동작 확인**

Run: `bash apex_tools/queue-lock.sh build debug`
Expected:
- `[queue-lock] lock acquired: build` 출력
- CMake configure + build + test 실행
- `[queue-lock] build completed successfully` 출력
- `%LOCALAPPDATA%/apex-build-queue/logs/{branch}.log`에 로그 기록
- lock 디렉토리 자동 삭제

- [ ] **Step 4: 수동 검증 — 개별 타겟 빌드**

Run: `bash apex_tools/queue-lock.sh build debug --target apex_core`
Expected: apex_core 타겟만 빌드

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/queue-lock.sh build.bat
git commit -m "feat(tools): build 채널 구현 — lock + build.bat 래핑 + 로그 tee"
```

---

## Task 6: merge 채널 구현

acquire/release/status 패턴의 merge 채널을 구현한다.

**Files:**
- Modify: `apex_tools/queue-lock.sh`
- Modify: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: merge 채널 테스트 추가**

`test-queue-lock.sh`에 추가:

```bash
# ── Test 8: merge acquire/release ──
echo "[Test 8] merge acquire + release"
rm -rf "$TEST_DIR/merge.lock" "$TEST_DIR/merge.owner" "$TEST_DIR/merge-queue/"*

# acquire (백그라운드에서 실행 — 바로 획득되어야 함)
APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" merge acquire &
merge_pid=$!

# lock 파일 생성을 폴링으로 대기 (타이밍 의존성 제거)
for i in $(seq 1 10); do
    [[ -d "$TEST_DIR/merge.lock" ]] && break
    sleep 0.5
done

assert_dir_exists "$TEST_DIR/merge.lock" "merge lock 획득"
owner_status=$(grep '^STATUS=' "$TEST_DIR/merge.owner" | cut -d= -f2)
assert_eq "merge status는 MERGING" "MERGING" "$owner_status"

# release
APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" merge release
assert_no_dir "$TEST_DIR/merge.lock" "merge lock 해제"

wait "$merge_pid" 2>/dev/null || true

# ── Test 9: merge status 출력 ──
echo "[Test 9] merge status — lock 없을 때"
rm -rf "$TEST_DIR/merge.lock" "$TEST_DIR/merge.owner"
status_output=$(APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" merge status)
echo "$status_output" | grep -q "LOCK=FREE"
assert_eq "LOCK=FREE 출력" "0" "$?"
```

- [ ] **Step 2: 테스트 실행하여 실패 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: FAIL

- [ ] **Step 3: merge 채널 구현**

`queue-lock.sh`의 merge 관련 함수:

```bash
do_merge() {
    local subcmd="${1:-help}"
    shift || true

    case "$subcmd" in
        acquire) do_merge_acquire ;;
        release) do_merge_release ;;
        status)  do_merge_status ;;
        conflict) do_merge_conflict ;;
        *)       echo "Usage: queue-lock.sh merge <acquire|release|status|conflict>"; exit 1 ;;
    esac
}

do_merge_acquire() {
    echo "[queue-lock] merge lock requested: branch=$BRANCH_ID"
    acquire_lock "merge"

    # STATUS를 MERGING으로 갱신
    sed -i 's/^STATUS=.*/STATUS=MERGING/' "$QUEUE_DIR/merge.owner" 2>/dev/null || true

    echo "[queue-lock] merge lock acquired. Proceed with merge workflow."
    echo "[queue-lock] IMPORTANT: run 'queue-lock.sh merge release' when done."

    # acquire/release 패턴: trap을 "경고만 출력"으로 변경
    # 에이전트가 release를 호출해야 하지만, 비정상 종료 시 안전망으로 자동 해제
    trap 'echo "[queue-lock] WARNING: merge lock auto-released on exit (did you forget merge release?)"; release_lock "merge"' EXIT
}

do_merge_release() {
    echo "[queue-lock] releasing merge lock"
    release_lock "merge"
    echo "[queue-lock] merge lock released"
}

do_merge_conflict() {
    local owner_file="$QUEUE_DIR/merge.owner"
    if [[ -f "$owner_file" ]]; then
        sed -i 's/^STATUS=.*/STATUS=CONFLICT/' "$owner_file"
        echo "[queue-lock] merge status set to CONFLICT"
    else
        echo "[queue-lock] ERROR: no merge lock owner file" >&2
        exit 1
    fi
}

do_merge_status() {
    local lock_dir="$QUEUE_DIR/merge.lock"
    local owner_file="$QUEUE_DIR/merge.owner"
    local queue_dir="$QUEUE_DIR/merge-queue"

    if [[ -d "$lock_dir" && -f "$owner_file" ]]; then
        echo "LOCK=HELD"
        grep '^BRANCH=' "$owner_file" | sed 's/BRANCH=/OWNER=/'
        grep '^PID=' "$owner_file"
        grep '^ACQUIRED=' "$owner_file"
        grep '^STATUS=' "$owner_file"
    else
        echo "LOCK=FREE"
        echo "STATUS=IDLE"
    fi

    local depth
    depth=$(ls "$queue_dir" 2>/dev/null | wc -l | tr -d ' ')
    echo "QUEUE_DEPTH=$depth"

    if [[ "$depth" -gt 0 ]]; then
        local entries
        entries=$(ls "$queue_dir" 2>/dev/null | sort | tr '\n' ',')
        echo "QUEUE=${entries%,}"
    fi
}
```

- [ ] **Step 4: 테스트 실행하여 통과 확인**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: PASS (전체)

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/queue-lock.sh apex_tools/tests/test-queue-lock.sh
git commit -m "feat(tools): merge 채널 구현 — acquire/release/status/conflict"
```

---

## Task 7: PreToolUse hooks 구현

빌드 도구 직접 호출과 lock 미획득 머지를 차단하는 hook을 구현한다.

**Files:**
- Create: `.claude/hooks/validate-build.sh`
- Create: `.claude/hooks/validate-merge.sh`
- Modify: `.claude/settings.json`

- [ ] **Step 1: 빌드 차단 hook 스크립트 작성**

```bash
#!/usr/bin/env bash
# .claude/hooks/validate-build.sh
# PreToolUse hook: 빌드 도구 직접 호출 차단

INPUT=$(cat)
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')

[[ -z "$COMMAND" ]] && exit 0

# queue-lock.sh를 통한 호출은 허용
if echo "$COMMAND" | grep -q 'queue-lock\.sh'; then
    exit 0
fi

# 차단 대상 패턴
BLOCKED_PATTERNS=(
    'cmake --build'
    'cmake --preset'
    '\bninja\b'
    '\bmsbuild\b'
    '\bcl\.exe\b'
    'build\.bat'
)

for pattern in "${BLOCKED_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -qE "$pattern"; then
        echo "차단: 빌드는 queue-lock.sh build를 통해서만 실행할 수 있습니다. (matched: $pattern)" >&2
        exit 2
    fi
done

exit 0
```

- [ ] **Step 2: 머지 차단 hook 스크립트 작성**

```bash
#!/usr/bin/env bash
# .claude/hooks/validate-merge.sh
# PreToolUse hook: lock 미획득 상태에서 gh pr merge 차단

INPUT=$(cat)
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // empty')
CWD=$(echo "$INPUT" | jq -r '.cwd // empty')

[[ -z "$COMMAND" ]] && exit 0

# gh pr merge 가 아니면 무시
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
    # CWD에서 프로젝트 디렉토리명 추출
    PROJECT_DIR=$(basename "$CWD")
    # 프로젝트 루트가 아니라 하위 디렉토리에서 실행될 수 있으므로
    # owner와 현재 디렉토리에 프로젝트 식별자가 포함되어 있는지 확인
    if [[ -n "$OWNER_BRANCH" ]] && ! echo "$CWD" | grep -q "$OWNER_BRANCH"; then
        echo "차단: merge lock 소유자가 $OWNER_BRANCH입니다 (현재: $CWD). 먼저 queue-lock.sh merge acquire를 실행하세요." >&2
        exit 2
    fi
fi

exit 0
```

- [ ] **Step 3: settings.json에 PreToolUse hook 등록**

`.claude/settings.json`에 PreToolUse 섹션 추가:

```json
{
  "extraKnownMarketplaces": { ... },
  "hooks": {
    "SessionStart": [ ... ],
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "bash .claude/hooks/validate-build.sh",
            "timeout": 5
          },
          {
            "type": "command",
            "command": "bash .claude/hooks/validate-merge.sh",
            "timeout": 5
          }
        ]
      }
    ]
  }
}
```

- [ ] **Step 4: hook 스크립트에 실행 권한 부여**

```bash
chmod +x .claude/hooks/validate-build.sh
chmod +x .claude/hooks/validate-merge.sh
```

- [ ] **Step 5: 수동 검증 — 빌드 차단 테스트**

Claude Code 세션에서 `cmake --build build/Windows/debug` 직접 실행 시도.
Expected: "차단: 빌드는 queue-lock.sh build를 통해서만 실행할 수 있습니다." 메시지와 함께 차단.

- [ ] **Step 6: 수동 검증 — 머지 차단 테스트**

Claude Code 세션에서 lock 없이 `gh pr merge --squash --admin` 실행 시도.
Expected: "차단: 먼저 queue-lock.sh merge acquire를 실행하세요." 메시지와 함께 차단.

- [ ] **Step 7: 커밋**

```bash
git add .claude/hooks/validate-build.sh .claude/hooks/validate-merge.sh .claude/settings.json
git commit -m "feat(hooks): PreToolUse hook — 빌드/머지 우회 차단"
```

---

## Task 8: CLAUDE.md 규칙 갱신

빌드 명령과 머지 프로세스 규칙을 갱신한다.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: 빌드 섹션 갱신**

`CLAUDE.md`의 `## 빌드` 섹션에서 빌드 명령을 `queue-lock.sh`로 변경:

기존:
```
- `cmd.exe //c "<프로젝트루트절대경로>\\build.bat" debug` (bash 셸에서, `//c` 필수). **반드시 절대 경로 사용** — `pwd`나 git root로 경로를 구한 뒤 조합. 상대 경로(`build.bat`)는 백그라운드 실행 시 작업 디렉토리 불일치로 실패함
```

변경:
```
- `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug` (bash 셸). **반드시 절대 경로 사용** — `pwd`나 git root로 경로를 구한 뒤 조합
- 개별 타겟 빌드: `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug --target apex_core`
- `build.bat` 직접 호출 금지 (PreToolUse hook이 차단)
- `cmake`, `ninja` 등 빌드 도구 직접 호출 금지 (PreToolUse hook이 차단)
```

- [ ] **Step 2: 머지 섹션에 merge-lock 규칙 추가**

`## 빌드` 섹션의 빌드 규칙 이후, `### Git / 브랜치` 섹션의 머지 규칙에 추가:

기존:
```
- **머지**: 리뷰 이슈 0건 → `gh pr merge --squash --admin`
```

변경:
```
- **머지**: 리뷰 이슈 0건 → 아래 순서로 실행:
  1. `queue-lock.sh merge acquire` (lock 획득까지 대기)
  2. `git fetch origin main && git rebase origin/main` (충돌 시 에이전트가 resolve)
  3. `queue-lock.sh build debug` (빌드 + 테스트 재검증)
  4. `git push --force-with-lease`
  5. `gh pr merge --squash --admin`
  6. `queue-lock.sh merge release`
- **머지 lock 없이 `gh pr merge` 실행 금지** (PreToolUse hook이 차단)
```

- [ ] **Step 3: 커밋**

```bash
git add CLAUDE.md
git commit -m "docs(rules): 빌드/머지 명령 queue-lock.sh 전환 + PreToolUse hook 규칙"
```

---

## Task 9: 통합 검증 + 엣지 케이스 테스트

전체 시스템의 end-to-end 동작을 검증한다.

**Files:**
- Modify: `apex_tools/tests/test-queue-lock.sh`

- [ ] **Step 1: 통합 테스트 추가**

```bash
# ── Test 10: build 채널 E2E (실제 build.bat 실행 없이 echo로 대체) ──
echo "[Test 10] build 채널 E2E (dry-run)"

# build.bat 대신 echo 스크립트로 테스트
mkdir -p "$TEST_DIR/fake-project/apex_tools"
cp "$QUEUE_LOCK" "$TEST_DIR/fake-project/apex_tools/queue-lock.sh"
cat > "$TEST_DIR/fake-project/build.bat" << 'BATCH'
@echo off
echo FAKE BUILD OK: %*
BATCH

APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$TEST_DIR/fake-project/apex_tools/queue-lock.sh" build debug 2>&1 | tail -1
assert_no_dir "$TEST_DIR/build.lock" "build 후 lock 자동 해제"
assert_file_exists "$TEST_DIR/logs/fake-project.log" "빌드 로그 생성"

# ── Test 11: stale lock + 큐 우선순위 E2E ──
echo "[Test 11] stale lock + 큐 우선순위"
rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner" "$TEST_DIR/build-queue/"*

# 죽은 PID로 stale lock 생성
mkdir -p "$TEST_DIR/build.lock"
echo -e "PID=99999\nBRANCH=dead_branch\nACQUIRED=0" > "$TEST_DIR/build.owner"

# 내 큐 파일 생성
touch "$TEST_DIR/build-queue/20260318_100000_test_$$"
MY_QUEUE_FILE="$TEST_DIR/build-queue/20260318_100000_test_$$"

# acquire 시 stale 감지 → lock 탈취 → 내 차례
APEX_BUILD_QUEUE_DIR="$TEST_DIR" BRANCH_ID="test" "$QUEUE_LOCK" _acquire_lock build &
acq_pid=$!
sleep 2
assert_dir_exists "$TEST_DIR/build.lock" "stale 감지 후 lock 재획득"

# 정리
APEX_BUILD_QUEUE_DIR="$TEST_DIR" "$QUEUE_LOCK" _release_lock build
wait "$acq_pid" 2>/dev/null || true
```

- [ ] **Step 2: 동시 접근 테스트 추가**

```bash
# ── Test 12: 동시 lock 획득 — 하나만 성공 ──
echo "[Test 12] 동시 lock 획득 경합"
rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner" "$TEST_DIR/build-queue/"*

# 2개 프로세스가 동시에 lock 획득 시도
(
    touch "$TEST_DIR/build-queue/20260318_100000_proc_a_$$"
    MY_QUEUE_FILE="$TEST_DIR/build-queue/20260318_100000_proc_a_$$"
    APEX_BUILD_QUEUE_DIR="$TEST_DIR" BRANCH_ID="proc_a" "$QUEUE_LOCK" _try_lock build && echo "A_GOT_LOCK" > "$TEST_DIR/result_a"
) &
pid_a=$!

(
    touch "$TEST_DIR/build-queue/20260318_100001_proc_b_$$"
    MY_QUEUE_FILE="$TEST_DIR/build-queue/20260318_100001_proc_b_$$"
    APEX_BUILD_QUEUE_DIR="$TEST_DIR" BRANCH_ID="proc_b" "$QUEUE_LOCK" _try_lock build && echo "B_GOT_LOCK" > "$TEST_DIR/result_b"
) &
pid_b=$!

wait "$pid_a" 2>/dev/null || true
wait "$pid_b" 2>/dev/null || true

# 정확히 하나만 lock을 획득해야 함
lock_count=0
[[ -f "$TEST_DIR/result_a" ]] && ((lock_count++))
[[ -f "$TEST_DIR/result_b" ]] && ((lock_count++))
assert_eq "동시 시도 중 정확히 1개만 lock 획득" "1" "$lock_count"

rm -rf "$TEST_DIR/build.lock" "$TEST_DIR/build.owner" "$TEST_DIR/result_a" "$TEST_DIR/result_b"
```

- [ ] **Step 3: PreToolUse hook 자동 테스트 추가**

```bash
# ── Test 13: validate-build.sh hook 차단 테스트 ──
echo "[Test 13] PreToolUse hook — 빌드 차단"
HOOK_SCRIPT="$SCRIPT_DIR/../../.claude/hooks/validate-build.sh"
if [[ -f "$HOOK_SCRIPT" ]]; then
    # cmake --build → 차단 (exit 2)
    result=$(echo '{"tool_input":{"command":"cmake --build build/Windows/debug"}}' | bash "$HOOK_SCRIPT" 2>/dev/null; echo $?)
    assert_eq "cmake --build 차단" "2" "$result"

    # queue-lock.sh build → 허용 (exit 0)
    result=$(echo '{"tool_input":{"command":"bash apex_tools/queue-lock.sh build debug"}}' | bash "$HOOK_SCRIPT" 2>/dev/null; echo $?)
    assert_eq "queue-lock.sh 허용" "0" "$result"

    # ninja → 차단 (exit 2)
    result=$(echo '{"tool_input":{"command":"ninja -C build"}}' | bash "$HOOK_SCRIPT" 2>/dev/null; echo $?)
    assert_eq "ninja 차단" "2" "$result"
else
    echo "  SKIP: validate-build.sh not found (Task 7 미완)"
fi

# ── Test 14: validate-merge.sh hook 차단 테스트 ──
echo "[Test 14] PreToolUse hook — 머지 차단"
MERGE_HOOK="$SCRIPT_DIR/../../.claude/hooks/validate-merge.sh"
if [[ -f "$MERGE_HOOK" ]]; then
    # lock 없이 gh pr merge → 차단 (exit 2)
    rm -rf "$TEST_DIR/merge.lock"
    result=$(echo '{"tool_input":{"command":"gh pr merge --squash --admin"},"cwd":"/test"}' | APEX_BUILD_QUEUE_DIR="$TEST_DIR" bash "$MERGE_HOOK" 2>/dev/null; echo $?)
    assert_eq "lock 없이 merge 차단" "2" "$result"
else
    echo "  SKIP: validate-merge.sh not found (Task 7 미완)"
fi
```

- [ ] **Step 4: 전체 테스트 실행**

Run: `bash apex_tools/tests/test-queue-lock.sh`
Expected: 전체 PASS

- [ ] **Step 3: 실제 빌드 테스트 (background)**

Run: `bash apex_tools/queue-lock.sh build debug` (run_in_background: true)
Expected:
- lock 획득 → 빌드 실행 → 테스트 통과 → lock 해제
- `%LOCALAPPDATA%/apex-build-queue/logs/` 에 로그 파일 생성

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/tests/test-queue-lock.sh
git commit -m "test(tools): queue-lock.sh 통합 테스트 추가"
```

---

## 최종 체크리스트

- [ ] `queue-lock.sh build debug` — 전체 빌드 동작 확인
- [ ] `queue-lock.sh build debug --target apex_core` — 개별 타겟 동작 확인
- [ ] `queue-lock.sh merge acquire/release/status` — 머지 lock 동작 확인
- [ ] stale lock 감지 — 죽은 PID lock 자동 정리 확인
- [ ] 고아 큐 엔트리 — 자동 정리 확인
- [ ] 동시 접근 — 2개 프로세스 중 1개만 lock 획득 확인
- [ ] PreToolUse hook — `cmake --build` 직접 호출 차단 확인
- [ ] PreToolUse hook — lock 없이 `gh pr merge` 차단 확인
- [ ] PreToolUse hook — `queue-lock.sh` 경유 호출은 허용 확인
- [ ] merge acquire 비정상 종료 시 — trap으로 lock 자동 해제 확인
- [ ] CLAUDE.md — 빌드/머지 규칙 갱신 확인
- [ ] 전체 테스트 스크립트 통과 확인 (14개 테스트)
