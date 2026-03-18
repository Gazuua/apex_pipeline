# #55 빌드 큐잉 + 머지 직렬화 시스템 — 설계 스펙

## 개요

Windows 로컬 환경에서 물리적으로 분리된 복수 브랜치 디렉토리(branch_01~03)가 동일 PC 자원을 공유할 때 발생하는 두 가지 경합 문제를 해결하는 중앙 큐잉 시스템.

- **빌드 큐잉**: MSVC+Ninja가 풀코어를 사용하므로 동시 빌드 시 시스템 렉 → FIFO 큐로 직렬화
- **머지 직렬화**: 동시 squash merge 시 공통 파일 충돌 → lock + rebase 선행으로 순차 머지 보장

## 핵심 원칙

> **정규 경로 하나만 열고, 나머지는 전부 막는다.**

| 작업 | 정규 경로 | 강제 메커니즘 |
|------|-----------|-------------|
| 빌드 | `queue-lock.sh build` | bash 래퍼가 lock 관리 + PreToolUse hook이 cmake/ninja/build.bat 직접 실행 차단 |
| 머지 | `queue-lock.sh merge` → 에이전트 작업 → `gh pr merge` | PreToolUse hook이 lock 미획득 상태에서 `gh pr merge` 차단 |

## 공유 인프라

### 경로

- **기본**: `%LOCALAPPDATA%\apex-build-queue\`
- **오버라이드**: 환경변수 `APEX_BUILD_QUEUE_DIR` 설정 시 해당 경로 사용
- **설정 가능한 환경변수**:
  - `APEX_BUILD_QUEUE_DIR` — 큐/lock 디렉토리 경로
  - `APEX_STALE_TIMEOUT_SEC` — stale lock 판정 시간 (기본 3600초 = 60분)
  - `APEX_POLL_INTERVAL_SEC` — 폴링 간격 (기본 1초)
- **선정 근거**: Windows 앱 로컬 데이터 표준 경로. OS 디스크 정리 대상 아님. 리부트에도 유지되어 stale lock 감지 가능. 프로젝트 디렉토리 바깥이라 복수 브랜치에서 자연스럽게 공유.

### 디렉토리 구조

```
%LOCALAPPDATA%\apex-build-queue\
├── build.lock/             # 빌드 뮤텍스 (mkdir atomic)
├── build.owner             # PID, 브랜치명, 획득 시각
├── build-queue/            # 빌드 FIFO 대기열
│   ├── 20260318_143012_branch_01_12345
│   └── 20260318_143025_branch_03_23456
├── merge.lock/             # 머지 뮤텍스 (mkdir atomic)
├── merge.owner             # PID, 브랜치명, 획득 시각
├── merge-queue/            # 머지 FIFO 대기열
│   └── 20260318_150000_branch_02_34567
└── logs/                   # 브랜치별 빌드 로그
    ├── branch_01.log
    ├── branch_02.log
    └── branch_03.log
```

### 용량

lock은 빈 디렉토리, owner는 수십 바이트 텍스트, 큐 파일도 빈 파일. 빌드 로그는 1회당 10~50KB (한 달 풀 사용 시 ~15MB). GB 단위 축적 불가.

## 통합 스크립트: `queue-lock.sh`

### 설계 철학

큐/lock 관심사를 단일 bash 스크립트에 집중하고, 실제 작업 도구(`build.bat`, `gh pr merge`)는 순수 기능만 담당. 채널 기반 확장 — 새로운 lock이 필요하면 채널만 추가.

### 인터페이스

```bash
apex_tools/queue-lock.sh <채널> <서브커맨드> [인자...]

# 빌드 (run-and-release 패턴: lock → 실행 → unlock 자동)
queue-lock.sh build debug                       # 전체 빌드
queue-lock.sh build debug --target apex_core    # 개별 타겟 빌드

# 머지 (acquire/release 패턴: 에이전트가 중간 작업)
queue-lock.sh merge acquire
queue-lock.sh merge release
queue-lock.sh merge status
```

### 프로젝트 루트 자동 감지

```bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
# build 채널 실행 시:
cmd.exe //c "${PROJECT_ROOT}\\build.bat" "${BUILD_ARGS[@]}"
```

각 브랜치 디렉토리의 `queue-lock.sh`가 자기 프로젝트의 `build.bat`을 절대 경로로 호출. 경로 불일치 문제 원천 해결.

### 브랜치 식별

프로젝트 루트 디렉토리명에서 추출: `basename "$PROJECT_ROOT"` → `apex_pipeline_branch_01` 등. git 브랜치명이 아닌 물리 디렉토리명 사용 — 복수 클론 간 고유성 보장.

## FIFO 큐

### 큐 파일명 포맷

`{타임스탬프}_{브랜치명}_{PID}`

- **타임스탬프**: `date +%Y%m%d_%H%M%S` (bash, 초 단위)
- **브랜치명**: 디렉토리 기반 식별자 (apex_pipeline_branch_01 등)
- **PID**: 요청 프로세스의 PID (동일 브랜치 복수 요청 구분)

### 동작 순서

1. 에이전트 도착 → 큐 디렉토리에 빈 파일 생성 (대기열 등록)
2. 폴링 루프 (1초 간격):
   - `ls | sort` → 내 파일이 첫 번째인가?
   - YES + lock 없음 → lock 획득 시도 (`mkdir`), 성공 시 owner 기록, 큐 파일 삭제, 작업 시작
   - NO → "대기 중... 앞에 N개 작업, 현재: 브랜치 X (PID Y)" 출력 + 10초 대기
3. 작업 완료 → lock 해제 (`rmdir` + owner 삭제)

### Atomic Lock

`mkdir`을 lock 메커니즘으로 사용. NTFS에서 `mkdir`은 atomic 연산 — 이미 존재하면 실패. 큐 순서 확인 → `mkdir` 시도 사이의 TOCTOU 경합을 방지.

```bash
# lock 획득 시도
if mkdir "$QUEUE_DIR/${channel}.lock" 2>/dev/null; then
    # 성공 — owner 파일 기록
else
    # 실패 — 다른 프로세스가 선점
fi
```

### Tiebreaker

동일 초에 도착한 경우 브랜치명 → PID 순 사전순 정렬로 결정. 3개 에이전트가 동일 초에 도착할 확률은 극히 낮으나, 발생 시에도 결정론적 순서 보장.

## Stale 감지

### Lock Stale 감지

PID + 타임스탬프 이중 검증:

1. **1차 — PID 생존 확인**: owner 파일의 PID로 프로세스 존재 여부 확인. 죽었으면 stale.
   - **Windows**: `tasklist /FI "PID eq $PID"` 사용 (`kill -0`은 Git Bash에서 네이티브 Windows 프로세스 감지 불가)
   - **Linux**: `kill -0 $PID`
2. **2차 — 타임스탬프 만료**: PID가 살아있어도 획득 시각이 최대 유효 시간 초과 시 stale. PID 재사용 방어.
   - 기본값: 60분 (빌드/머지 통일)
   - 환경변수 `APEX_STALE_TIMEOUT_SEC`로 오버라이드 가능 (초 단위)
3. **복구**: stale 판정 시 기존 lock 디렉토리 + owner 삭제 + 콘솔에 경고 출력. 큐 순서대로 다음 에이전트가 획득.
4. **수동 복구**: 자동 감지가 실패하는 극단적 상황(PID 재사용 + 타임아웃 미만)에서는 `rmdir "${QUEUE_DIR}/build.lock"` 또는 `rmdir "${QUEUE_DIR}/merge.lock"`으로 수동 해제.

### 큐 엔트리 Stale 감지

큐 파일에도 PID가 포함되어 있으므로 동일한 PID 생존 확인 적용. 폴링 루프에서 큐 목록을 읽을 때:

1. 앞에 있는 큐 파일의 PID가 죽어있으면 → 해당 큐 파일 삭제 (고아 엔트리 정리)
2. 내 앞의 고아 엔트리가 모두 정리되면 내 순서가 됨

크래시로 큐 파일이 잔존해도 영구 블로킹 방지.

## 빌드 큐잉

### 구조

`queue-lock.sh`가 lock/큐를 관리하고, `build.bat`은 순수 빌드만 담당.

### 흐름

```
queue-lock.sh build debug [--target ...]
  → 큐 디렉토리 초기화 (없으면 생성)
  → 큐에 등록 (타임스탬프_브랜치명_PID 파일 생성)
  → 폴링 루프:
      고아 큐 엔트리 정리 (PID 확인)
      stale lock 확인 + 정리
      내 순서 + mkdir lock 성공? → owner 기록, 큐 파일 삭제
      아니면 → 상태 출력 + 10초 대기
  → cmd.exe //c "${PROJECT_ROOT}\build.bat" debug [--target ...]
      빌드 로그를 logs/{branch}.log에 tee (콘솔 + 파일 동시 출력)
  → lock 해제 (trap EXIT로 정상/에러/시그널 모든 경로에서 보장)
```

### build.bat 변경

`build.bat`에 추가 인자 전달 지원:

```batch
:: 기존
cmake --build "build/Windows/%PRESET%"

:: 변경 — 추가 인자 전달 (shift 기반, 인자 수 제한 없음)
cmake --build "build/Windows/%PRESET%" %EXTRA_ARGS%
```

`build.bat` 자체에는 lock 로직을 넣지 않음. 순수 빌드 전용.

### 에이전트 규칙

- `queue-lock.sh build debug`로 호출 (`run_in_background: true` + 타임아웃 없음)
- 개별 타겟: `queue-lock.sh build debug --target apex_core`
- `build.bat` 직접 호출 금지 (PreToolUse hook에서 차단)
- 큐 대기는 스크립트 내부에서 처리 → 에이전트 토큰 소비 없음

## 머지 직렬화

### 인터페이스

```bash
queue-lock.sh merge acquire    # 큐 등록 + lock 획득까지 폴링 대기
queue-lock.sh merge release    # lock 해제
queue-lock.sh merge status     # 현재 상태 조회
```

### status 출력 포맷

```
LOCK=HELD|FREE
OWNER=apex_pipeline_branch_02
PID=12345
ACQUIRED=2026-03-18T15:00:00
STATUS=MERGING|CONFLICT|IDLE
QUEUE_DEPTH=2
QUEUE=apex_pipeline_branch_01_67890,apex_pipeline_branch_03_11111
```

에이전트가 파싱하기 쉬운 KEY=VALUE 형식.

### 머지 프로세스

에이전트가 수행하는 전체 머지 흐름:

```
1. queue-lock.sh merge acquire          ← 스크립트 (큐 등록 + lock 획득까지 폴링)
2. git fetch origin main                ← 에이전트
3. git rebase origin/main               ← 에이전트
4. (충돌 시) 에이전트가 자체 resolve     ← 작업 컨텍스트 보유한 에이전트가 최적
5. queue-lock.sh build debug            ← 빌드 + 테스트 재검증 (빌드 lock도 획득)
6. git push --force-with-lease          ← 에이전트
7. gh pr merge --squash --admin         ← 에이전트
8. queue-lock.sh merge release          ← 스크립트
```

### CONFLICT 처리

에이전트가 rebase 충돌 resolve 실패 시:

1. merge.owner에 `STATUS=CONFLICT` 기록
2. lock 유지 (후속 에이전트가 불완전한 상태에서 작업하는 것 방지)
3. 후속 에이전트는 폴링 중 CONFLICT 감지 → 대기 계속
4. 충돌 해결 후 STATUS를 MERGING으로 복원 → 프로세스 계속

### Lock 해제 보장

`trap` 기반 자동 정리:

```bash
cleanup() {
    rmdir "$QUEUE_DIR/${channel}.lock" 2>/dev/null
    rm -f "$QUEUE_DIR/${channel}.owner"
    rm -f "$MY_QUEUE_FILE"  # 잔여 큐 파일도 정리
}
trap cleanup EXIT
```

정상 종료, 에러, SIGTERM/SIGINT 모든 경로에서 lock 해제 보장. Windows batch에는 trap이 없으므로 lock 관리를 bash 스크립트에서 담당하는 핵심 이유.

**Windows 제약**: `cmd.exe` 서브프로세스가 강제 종료될 때 bash의 trap이 발동하지 않을 수 있음. 이 경우 stale lock 감지(PID 확인 + 타임스탬프 만료)가 최종 안전망으로 동작. 수동 복구: `rmdir "${QUEUE_DIR}/${channel}.lock"`.

## PreToolUse Hook 강제

### 빌드 차단

lock 없이 빌드 도구를 직접 호출하는 것을 원천 차단:

- **차단 대상**: `cmake --build`, `cmake --preset`, `ninja`, `msbuild`, `cl.exe`, `build.bat` 직접 호출
- **허용**: `queue-lock.sh build` (유일한 정규 빌드 경로)
- **차단 시 안내**: "빌드는 queue-lock.sh build를 통해서만 실행할 수 있습니다."
- **구현**: Claude Code `settings.json`의 PreToolUse hook에 Bash 도구 입력 패턴 매칭. 구현 시 Claude Code hook API 탐색 후 구체적 설정 확정 (hook이 지원하는 패턴 매칭 방식, 차단/허용 응답 구조 등)

### 머지 차단

lock 미획득 상태에서 머지를 시도하는 것을 원천 차단:

- **차단 조건**: Bash 명령에 `gh pr merge`가 포함될 때, `merge.lock/`이 없거나 owner의 브랜치가 현재 브랜치가 아니면 거부
- **허용**: `queue-lock.sh merge acquire` 후 lock 소유 상태에서만 `gh pr merge` 실행 가능
- **차단 시 안내**: "먼저 queue-lock.sh merge acquire를 실행하세요."
- **구현**: hook 스크립트가 `merge.owner` 파일을 읽어서 소유자 검증. 구현 시 Claude Code hook API와 함께 구체적 설정 확정

### 구현 참고

PreToolUse hook은 Claude Code의 hook 시스템을 사용. 구현 시점에 Claude Code 공식 문서를 참조하여 hook 설정 구조(`settings.json` 또는 `settings.local.json`)를 확정한다. 현재 프로젝트에는 `SessionStart` hook만 설정되어 있으므로, PreToolUse hook 추가는 별도 검증 필요.

## 산출물

| 파일 | 작업 | 설명 |
|------|------|------|
| `apex_tools/queue-lock.sh` | 신규 | 통합 큐/lock 스크립트 (build/merge 채널) |
| `build.bat` | 수정 | 추가 인자(--target 등) 전달 지원. lock 로직 미포함 |
| hook 설정 | 신규 | PreToolUse hook 2개 (빌드 차단 + 머지 차단) |
| `CLAUDE.md` | 수정 | 빌드 명령 `queue-lock.sh build`로 변경, 머지 프로세스에 `queue-lock.sh merge` 필수 사용 추가 |
