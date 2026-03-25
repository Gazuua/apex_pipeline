# apex-agent — Go 백엔드 가이드

## 빌드

```bash
# Claude Code 세션 내 (go 직접 — 권장)
export PATH="/c/Program Files/Go/bin:$PATH"   # git-bash에서 go 경로 추가
go build -o apex-agent.exe ./cmd/apex-agent    # 빌드
go test ./... -count=1                         # 전체 테스트 (단위 + e2e)

# Makefile (bash/MSYS — make 있는 환경)
make build        # 빌드 + 자동 install
make test         # 전체 테스트 (단위 + e2e, -race -cover)

# Windows 더블클릭
build.bat         # 빌드 + 자동 install
build.bat test    # 전체 테스트 (단위 + e2e)
```

- **git-bash에 `make` 없음** — Claude Code 세션에서는 `go build` 직접 사용
- **validate-build hook이 `build.bat`/`go build` 허용** — Go 툴체인 커맨드는 C++ 빌드와 무관하므로 통과
- 설치 경로: `$LOCALAPPDATA/apex-agent/` (Windows) / `$HOME/.local/bin/` (Linux)
- `run-hook`이 설치 경로 바이너리를 우선 사용 → 빌드 후 즉시 hook에 반영

### 바이너리 설치 (수동)

```bash
# 데몬이 실행 중이면 먼저 종료해야 함 — Windows 파일 잠금
apex-agent daemon stop
cp apex-agent.exe "$LOCALAPPDATA/apex-agent/apex-agent.exe"
apex-agent daemon start
```

**주의**: 데몬 실행 중에 `cp`하면 "Device or resource busy" 에러. 반드시 stop → cp → start 순서.

## 크로스 플랫폼 hook 실행

- **문제**: Claude Code가 hook을 Node.js `spawn`으로 실행 → Windows에서 `.exe` 없이 바이너리를 못 찾음
- **해결**: `run-hook` bash 래퍼 스크립트가 OS에 맞는 바이너리를 자동 선택
- **규칙**: `settings.json`의 모든 hook 커맨드는 반드시 `bash ./apex_tools/apex-agent/run-hook` 경유
  ```json
  "command": "bash ./apex_tools/apex-agent/run-hook hook validate-build"
  ```
- **금지**: `./apex_tools/apex-agent/apex-agent` 또는 `./apex_tools/apex-agent/apex-agent.exe` 직접 호출 — 크로스 플랫폼 깨짐

## 데몬

상시 실행 백그라운드 프로세스. Named Pipe(Windows) / Unix Socket(Linux) IPC.

```bash
apex-agent daemon start   # 백그라운드 시작
apex-agent daemon stop    # 종료
apex-agent daemon status  # 상태 확인
apex-agent daemon run     # 포그라운드 (디버깅용)
```

- DB: `$LOCALAPPDATA/apex-agent/apex-agent.db` (SQLite)
- 로그: `$LOCALAPPDATA/apex-agent/logs/YYYYMMDD.log` (일별 분할, max_days=30 자동 정리)
- PID: `$LOCALAPPDATA/apex-agent/apex-agent.pid`
- 소켓: `\\.\pipe\apex-agent` (Windows) / `/tmp/apex-agent.sock` (Linux)
- SessionStart hook이 자동 기동 (`run-hook plugin setup`)
- **idle timeout 없음** — 데몬은 명시적 종료(`daemon stop`, IPC shutdown)까지 무기한 실행. SessionStart hook이 자동 기동하고, 크래시/수동 종료 시 IPC auto-restart가 자동 복구
- **IPC auto-restart** — hook/CLI가 데몬에 IPC 요청 시 연결 실패하면 자동으로 `ensureDaemon()` 호출 후 1회 재시도. 데몬 크래시, 수동 종료 등 모든 비가용 상황에서 투명하게 복구
- **데몬 관리 명령 hook 바이패스** — `daemon start/stop/status/run`, `plugin setup` 명령은 validate-handoff hook을 우회. 데몬 미실행 시에도 복구 경로가 차단되지 않음
- **maintenance lock** — `daemon stop`이 `apex-agent.maintenance` 파일을 생성하여 auto-restart 억제. `daemon start/run`이 해제. 바이너리 업그레이드 시 구 바이너리로 재시작되는 레이스 방지. 바이너리 업데이트 권장 순서: `daemon stop && go build && cp && daemon start` (단일 체인)

### HTTP 대시보드

데몬 내장 HTTP 서버. 브라우저에서 시스템 상태를 실시간 모니터링.

```bash
# 데몬 시작 시 자동 기동 (별도 명령 불필요)
# 브라우저에서 접속:
http://localhost:7600
```

- **4개 페이지**: Dashboard (요약), Backlog (필터/정렬/상세), Handoff (상태 머신), Queue (히스토리 이벤트 로그 — Build/Merge 좌우 분리, 시간 범위 필터, 무한 스크롤)
- **Go 템플릿 + HTMX** — 1초 폴링 실시간 갱신, 폴링 속도 조절 (Fast 0.5s / Normal 1s / Slow 2s)
- **JSON API**: `/api/backlog`, `/api/handoff`, `/api/queue`
- **설정**: `config.toml`의 `[http]` 섹션 (`enabled`, `addr`). 기본: `localhost:7600`
- 포트 충돌 시 경고 로그 + IPC만으로 계속 운영 (데몬 종료 안 함)
- localhost 바인딩 전용 — 인증 불필요

### 모듈 등록 순서

backlog → queue → handoff 순서 필수 (handoff가 backlog.Manager + queue.Manager를 참조):
```go
backlogMod := backlog.New(d.Store())
queueMod := queue.New(d.Store())
handoffMod := handoff.New(d.Store(), backlogMod.Manager(), queueMod.Manager(), backlogMod.Manager())
d.Register(hook.New())
d.Register(backlogMod)
d.Register(handoffMod)
d.Register(queueMod)
```

## Hook 게이트

4개 Bash hook + 2개 Edit/Write hook. `settings.json`에서 PreToolUse로 등록.

| Hook | 트리거 | 역할 | 차단 시 exit |
|------|--------|------|:---:|
| `validate-build` | Bash | cmake/ninja/build.bat/bench_* 직접 호출 차단 | 2 |
| `validate-merge` | Bash | PR 명령 검증: `gh pr merge` 직접 호출 전면 차단 + `--base main` 강제 | 2 |
| `validate-handoff` | Bash | 미등록 커밋 차단 + 머지 시 FIXING 백로그 차단 | 2 |
| `enforce-rebase` | Bash | push 전 자동 리베이스 | 2 |
| `handoff-probe` | Edit/Write | 미등록 편집 차단 + 상태별 소스 게이트 | 2 |
| `validate-backlog` | Edit/Write/Read | docs/BACKLOG.json 직접 접근 차단 | 2 |

### 브랜치 조회 순서 (hook)

1. `workspaceID(cwd)` — 프로젝트 디렉토리명에서 추출 (예: `branch_02`)
2. `gitCurrentBranch()` — `git branch --show-current` fallback
3. 둘 다 없으면: Bash hook은 통과, Edit/Write hook은 차단

daemon IPC `resolve-branch` 라우트가 이 로직을 서버 사이드에서 처리.

### validate-build 허용 목록

차단 패턴(`cmake`, `ninja`, `build.bat`, `bench_*` 등) 매칭 전에 허용 체크:

| 패턴 | 이유 |
|------|------|
| `queue-lock.sh` | 레거시 호환 |
| `apex-agent`, `run-hook` | Go 백엔드 래퍼 (queue build/benchmark 경유) |
| `go build/test/run/install` | Go 툴체인 (C++ 빌드와 무관) |
| `apex-agent/build.bat` | Go 빌드+자동 install (경로에 `apex-agent/` 포함 시 허용, 루트 build.bat은 차단) |
| `gh pr`, `gh run` | GitHub CLI 명령 |
| read-only 명령 (cat, grep 등) | 읽기 전용 명령 |

### stdin 프로토콜

- Claude Code가 hook에 JSON을 stdin으로 전달
- **반드시 `json.NewDecoder(os.Stdin).Decode()`** 사용 — `io.ReadAll`은 stdin 파이프가 안 닫혀서 무한 블록됨
- Bash hook: `tool_input.command` 필드
- Edit/Write hook: `tool_input.file_path` 필드

## 백로그 CLI

**stale import guard**: `UpdateFromImport`는 JSON의 `updated_at`과 DB의 `updated_at`을 비교한다. DB가 더 최신이면 import를 스킵(DB wins). 브랜치 간 JSON 버전 차이로 인한 데이터 원복을 방지.

```bash
apex-agent backlog add --title "..." --severity MAJOR --timeframe NOW --scope CORE --type BUG --description "..." --fix
apex-agent backlog add --title "..." --severity MAJOR --timeframe NOW --scope CORE --type BUG --description "..." --no-fix
apex-agent backlog fix ID [ID...]             # OPEN → FIXING + 현재 브랜치 연결
apex-agent backlog show ID                    # 개별 항목 전체 필드 key-value 출력
apex-agent backlog list [--timeframe NOW] [--severity MAJOR] [--status OPEN] [-v]
apex-agent backlog update ID --title "..." --severity MAJOR --description "..."
apex-agent backlog resolve ID --resolution FIXED
apex-agent backlog release ID --reason "..."
apex-agent backlog export                     # DB → docs/BACKLOG.json 직접 쓰기
apex-agent backlog export --stdout            # JSON stdout 출력 (디버깅용)
apex-agent backlog check ID

# 레거시 MD → DB 싱크 (최초 마이그레이션 시에만 필요)
apex-agent migrate backlog
```

### 중복 착수 방지 — `--fix` / `--no-fix` 강제

활성 브랜치에서 `backlog add` 시 `--fix` 또는 `--no-fix` 필수 (미지정 시 에러):
- `--fix`: 등록 즉시 FIXING 전이 + 현재 브랜치 연결 → "이 작업에서 바로 고친다"
- `--no-fix`: OPEN 유지 → "나중에 별도 작업으로 수정한다"
- 활성 브랜치 없는 상태에서는 강제 안 함 (기존 동작 유지)

작업 중 기존 OPEN 백로그를 고치기로 결정 변경 시: `backlog fix N [N...]`

### 데이터 관리 — Source of Truth

DB: Source of Truth (상태 + 메타데이터)
JSON: git 백업 (DB 유실 시 SyncImport로 복원)

- **DB가 전체의 출처** — 상태, 제목, 등급, 설명 등 모든 필드. 상태 변경은 CLI 전용
- **JSON은 git 백업** — DB 유실 시 SyncImport로 복원. export가 DB→JSON 전체 덮어쓰기
- **BACKLOG 파일 직접 접근 금지** — validate-backlog hook이 Read/Edit/Write 모두 차단. 조회는 `backlog list/show`, 수정은 `backlog add/update/resolve/release` CLI 사용

Import (SyncImport) — JSON(또는 레거시 MD) → DB:
- DB에 없는 항목 → 새로 추가
- DB에 있는 항목 → 메타데이터 갱신, 상태 불변
- 파일에서 삭제된 항목 → DB에 잔존

Export (`backlog export`) — DB → `docs/BACKLOG.json` 직접 쓰기:
- 전체 항목 (OPEN + FIXING + RESOLVED) 단일 JSON 파일
- import-first 안전장치 포함 (DB 유실 시 JSON에서 자동 복원)
- 레거시 MD 파일 존재 시 import 후 자동 삭제 (마이그레이션)

### 워크플로우별 타이밍

| 시점 | 명령 | 설명 |
|------|------|------|
| 착수 시 (①) | SyncImport (자동) | JSON → DB 메타데이터 싱크 (notify start가 내부 실행) |
| 작업 중 (③) | `backlog add/update/resolve/release` | CLI로 상태·메타데이터 변경 |
| 머지 시 (⑦) | `notify merge` 내부 자동 | export + commit → rebase → push → merge (에이전트 수동 export 불필요) |

### 머지 전 백로그 정리 (에이전트 수행)

`ValidateMergeGate`가 FIXING 백로그가 남아있으면 `notify merge`를 차단한다.
에이전트가 머지 전에 반드시 각 FIXING 백로그를 판단하여 처리해야 한다:

```bash
# 1. 연결된 FIXING 백로그 확인
apex-agent backlog list --status FIXING

# 2. 각 항목 판단
apex-agent backlog resolve ID --resolution FIXED       # 해결 완료한 건
apex-agent backlog release ID --reason "사유"           # 못 끝낸 건 → OPEN 복귀

# 3. FIXING 0건 확인 후 머지 진행 (lock→export→rebase→push→merge→finalize→checkout 자동 수행)
apex-agent handoff notify merge --summary "..."
```

### 열거형 검증 규칙

모든 열거형 필드는 **UPPER_SNAKE_CASE 필수**. 소문자/레거시 값은 즉시 거부.

| 필드 | 허용값 |
|------|--------|
| severity | `CRITICAL`, `MAJOR`, `MINOR` |
| timeframe | `NOW`, `IN_VIEW`, `DEFERRED` |
| type | `BUG`, `DESIGN_DEBT`, `TEST`, `DOCS`, `PERF`, `SECURITY`, `INFRA` |
| scope | `CORE`, `SHARED`, `GATEWAY`, `AUTH_SVC`, `CHAT_SVC`, `INFRA`, `CI`, `DOCS`, `TOOLS` |
| status | `OPEN`, `FIXING`, `RESOLVED` |
| resolution | `FIXED`, `DOCUMENTED`, `WONTFIX`, `DUPLICATE`, `SUPERSEDED` |

`enums.go`에 const + Validate/Normalize 함수 정의. `manage.go`의 `Add()`에서 입력 시점 검증.

## 핸드오프 CLI

```bash
# 백로그 연결 착수
apex-agent handoff notify start --backlog 126 --summary "..." --scopes core,shared [--skip-design]

# 비백로그 작업 착수
apex-agent handoff notify start job --branch-name feature/foo --summary "..." --scopes tools [--skip-design]

# 상태 전이
apex-agent handoff notify design --summary "..."
apex-agent handoff notify plan --summary "..."
apex-agent handoff notify merge --summary "..."   # 머지 유일 진입점 — 전체 파이프라인 실행

# 조회/확인
apex-agent handoff status
apex-agent handoff backlog-check N
```

### notify merge 내부 파이프라인

`handoff notify merge --summary "..."`는 **머지의 유일한 진입점**. 데몬이 다음을 원자적으로 수행:

| # | 단계 | 실패 시 |
|---|------|---------|
| ① | merge lock acquire | 대기 |
| ② | backlog import (non-fatal) + export + commit | 에러, lock release |
| ③ | git fetch + rebase origin/main | rebase --abort, 에러, lock release |
| ④ | git push --force-with-lease | 에러, lock release |
| ⑤ | gh pr merge --squash --delete-branch --admin | 에러, lock release |
| ⑥ | handoff finalize (active → history MERGED) | 에러 (exit 1) + 가이드 |
| ⑦ | checkout main + pull | 경고 (exit 0) + 가이드 |
| ⑧ | merge lock release (defer) | 항상 실행 |

## 큐 CLI

```bash
apex-agent queue build <preset> [extra args...]     # 빌드 잠금 획득 후 build.bat 실행
apex-agent queue benchmark <exe> [bench args...]    # 빌드 잠금 획득 후 벤치마크 실행
apex-agent queue acquire <channel>                  # 범용 채널 잠금 획득
apex-agent queue release <channel>                  # 범용 채널 잠금 해제
apex-agent queue status <channel>                   # 채널 상태 조회
```

- `queue build`와 `queue benchmark`는 동일한 "build" 채널 lock 공유 — 빌드/벤치마크 상호배제
- merge lock은 `handoff notify merge`가 내부에서 자동 관리 — 에이전트가 직접 acquire/release 불필요

## Cleanup CLI

```bash
apex-agent cleanup              # dry-run (기본) — 삭제 대상만 표시
apex-agent cleanup --execute    # 실제 삭제 수행
```

- **기본 동작이 dry-run** — 플래그 없이 실행하면 삭제 없이 대상만 출력. `--dry-run` 플래그는 없음
- 활성 핸드오프 브랜치 자동 보호 (active_branches 조회)
- CWD 브랜치 보호 (현재 체크아웃된 브랜치 삭제 안 함)
- 정리 대상: 머지 완료 로컬/리모트 브랜치, 고아 워크트리, 워크스페이스 복사본 로컬 브랜치 (같은 origin URL의 형제 디렉토리)

### 착수 필수 플래그

- `--summary`: **필수** — 작업 요약 없이 착수 불가
- `--backlog`: `notify start`에서 **필수** — 비백로그 작업은 `notify start job` 사용

### 상태 머신

`STARTED → DESIGN_NOTIFIED → IMPLEMENTING` (active 상태). merge/drop은 active에서 삭제 → history로 MERGED/DROPPED 이관.

- `--skip-design`: `STARTED` 스킵 → 바로 `IMPLEMENTING`
- `STARTED`/`DESIGN_NOTIFIED`: 소스 파일 편집 차단 (문서만 허용)
- `IMPLEMENTING`: 모든 파일 편집 허용

### junction 테이블 (branch_backlogs)

- `NotifyStart` 시 `backlog_ids` 슬라이스로 복수 백로그 연결
- 각 백로그는 `FIXING` 상태로 전이
- 머지 시점에 `FIXING` 백로그가 남아있으면 차단 (`ValidateMergeGate`)
- `release` 커맨드로 `FIXING → OPEN` 복귀 가능

### git_branch 컬럼

- `NotifyStart` 시 `git branch --show-current` 값을 `git_branch` 컬럼에 저장
- hook이 workspace ID로 못 찾으면 git_branch로 fallback 조회
- 다른 워크스페이스에서 같은 git branch를 체크아웃해도 등록된 브랜치를 찾을 수 있음

## 테스트

```bash
go test ./...                              # 전체
go test ./internal/modules/backlog/...     # 백로그 단위
go test ./internal/modules/handoff/...     # 핸드오프 단위
go test ./e2e/...                          # E2E 통합
go test ./e2e/... -run TestBacklog_Enum    # 특정 테스트
```

### E2E 테스트 구조

`e2e/testenv/env.go`가 인프로세스 데몬을 기동. 각 테스트가 독립 환경(DB + 소켓) 사용.

- 데몬 기동 대기: **polling 루프 (50ms × 100 = 5초)** — `time.Sleep` 하드코딩 금지
- `-race` 플래그: CI에서 필수, 로컬에서도 권장

### 마이그레이션

| 모듈 | 버전 | 내용 |
|------|:----:|------|
| backlog | v1 | backlog_items 테이블 생성 |
| backlog | v2 | AUTOINCREMENT 전환 |
| backlog | v3 | 대문자 정규화 + DEFAULT 'OPEN' |
| handoff | v1 | branches + notifications + acks 테이블 생성 |
| handoff | v2 | branch_backlogs junction + branches에서 backlog_id 제거 |
| handoff | v3 | branches.git_branch 컬럼 추가 |
| handoff | v4 | status UPPER_SNAKE_CASE 정규화 |
| handoff | v5 | branches → active_branches 리네이밍, branch_history 생성, MERGE_NOTIFIED 이관, git_branch UNIQUE 제약 |
| handoff | v6 | notifications + notification_acks 테이블 DROP (알림 시스템 제거) |
| queue | v1 | queue 테이블 생성 (channel, branch, pid, status) |
| queue | v2 | status UPPER_CASE 정규화 |
| queue | v3 | finished_at 컬럼 추가 |
| queue | v4 | queue_history 이벤트 로그 테이블 + 인덱스 |

daemon 시작 시 자동 실행. 롤백 미지원 — 항상 전진 마이그레이션.

### Store context 전파

`Querier` 인터페이스의 `Exec/Query/QueryRow`는 `ctx context.Context`를 첫 인자로 받는다.
IPC 핸들러 → Manager → Store 체인으로 ctx가 전파되어 graceful shutdown과 request timeout이 DB 쿼리까지 도달한다.
Dashboard 메서드(`Dashboard*`)는 `context.Background()` 사용 (짧은 읽기 전용, httpd querier 인터페이스 변경 불필요).
