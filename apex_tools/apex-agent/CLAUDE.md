# apex-agent — Go 백엔드 가이드

## 빌드

```bash
# Makefile (bash/MSYS — 권장)
make build        # 로컬 빌드
make test         # 테스트 (-race -cover)
make install      # $LOCALAPPDATA/apex-agent/ 에 설치

# Windows 더블클릭
build.bat         # 빌드
build.bat test    # 테스트
build.bat install # 설치
```

빌드 산출물: `apex-agent.exe` (Windows) / `apex-agent` (Linux). NTFS에서 두 이름은 같은 파일.

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
- 로그: `$LOCALAPPDATA/apex-agent/apex-agent.log`
- PID: `$LOCALAPPDATA/apex-agent/apex-agent.pid`
- 소켓: `\\.\pipe\apex-agent` (Windows) / `/tmp/apex-agent.sock` (Linux)
- SessionStart hook이 자동 기동 (`run-hook plugin setup`)

### 모듈 등록 순서

backlog → handoff 순서 필수 (handoff가 backlog.Manager를 참조):
```go
backlogMod := backlog.New(d.Store())
d.Register(backlogMod)
d.Register(handoff.New(d.Store(), backlogMod.Manager()))
```

## Hook 게이트

4개 Bash hook + 1개 Edit/Write hook. `settings.json`에서 PreToolUse로 등록.

| Hook | 트리거 | 역할 | 차단 시 exit |
|------|--------|------|:---:|
| `validate-build` | Bash | cmake/ninja/build.bat 직접 호출 차단 | 2 |
| `validate-merge` | Bash | merge lock 미획득 시 `gh pr merge` 차단 | 2 |
| `validate-handoff` | Bash | 미등록 커밋 차단 + 머지 시 미ack/FIXING 차단 | 2 |
| `enforce-rebase` | Bash | push 전 자동 리베이스 | 2 |
| `handoff-probe` | Edit/Write | 미등록 편집 차단 + 상태별 소스 게이트 | 2 |

### 브랜치 조회 순서 (hook)

1. `workspaceID(cwd)` — 프로젝트 디렉토리명에서 추출 (예: `branch_02`)
2. `gitCurrentBranch()` — `git branch --show-current` fallback
3. 둘 다 없으면: Bash hook은 통과, Edit/Write hook은 차단

daemon IPC `resolve-branch` 라우트가 이 로직을 서버 사이드에서 처리.

### stdin 프로토콜

- Claude Code가 hook에 JSON을 stdin으로 전달
- **반드시 `json.NewDecoder(os.Stdin).Decode()`** 사용 — `io.ReadAll`은 stdin 파이프가 안 닫혀서 무한 블록됨
- Bash hook: `tool_input.command` 필드
- Edit/Write hook: `tool_input.file_path` 필드

## 백로그 CLI

```bash
apex-agent backlog add --title "..." --severity MAJOR --timeframe NOW --scope CORE --type BUG --description "..."
apex-agent backlog list [--timeframe NOW] [--severity MAJOR] [--status OPEN]
apex-agent backlog resolve ID --resolution FIXED
apex-agent backlog release ID --reason "..."
apex-agent backlog export
apex-agent backlog check ID
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
apex-agent handoff notify start --backlog 126 --summary "..." [--skip-design] [--scopes core,shared]

# 비백로그 작업 착수
apex-agent handoff notify job --summary "..." [--skip-design]

# 상태 전이
apex-agent handoff notify design --summary "..."
apex-agent handoff notify plan --summary "..."
apex-agent handoff notify merge --summary "..."

# 조회/확인
apex-agent handoff status
apex-agent handoff check
apex-agent handoff ack --id N --action no-impact
apex-agent handoff backlog-check N
```

### 상태 머신

`started → design-notified → implementing → merge-notified`

- `--skip-design`: `started` 스킵 → 바로 `implementing`
- `started`/`design-notified`: 소스 파일 편집 차단 (문서만 허용)
- `implementing`: 모든 파일 편집 허용

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
| handoff | v1 | branches + notifications + acks 테이블 |
| handoff | v2 | branch_backlogs junction + branches에서 backlog_id 제거 |
| handoff | v3 | branches.git_branch 컬럼 추가 |

daemon 시작 시 자동 실행. 롤백 미지원 — 항상 전진 마이그레이션.
