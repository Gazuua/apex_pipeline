# apex-agent 강화: Config + Logging + E2E 테스트 설계 스펙

- **상태**: 설계 확정
- **백로그**: BACKLOG-126 (Phase 0~5 완료 후 강화)
- **스코프**: tools, infra
- **작성일**: 2026-03-22
- **선행**: `20260322_100656_apex_agent_design_spec.md`, Phase 0~5 구현 완료

---

## 1. 목표

apex-agent Go 백엔드를 프로덕션 투입 가능한 수준으로 강화한다.
세 가지 영역: Config 시스템, 로깅 시스템, E2E 통합 테스트.

---

## 2. 구현 순서

| # | 영역 | 이유 |
|---|------|------|
| 1 | **Config 시스템** | 로깅과 테스트가 config 기반으로 설정 주입 |
| 2 | **시스템 바이너리 설치 + 버전 프로토콜** | 멀티 워크스페이스 운영 기반 |
| 3 | **로깅 시스템** | E2E 실패 시 원인 추적 가능 |
| 4 | **E2E 테스트** | 인프라 준비 완료 후 검증 |

---

## 3. Config 시스템

### 결정 사항

| 항목 | 결정 |
|------|------|
| 포맷 | TOML (`github.com/BurntSushi/toml`) |
| 파일 위치 | `%LOCALAPPDATA%/apex-agent/config.toml` |
| 로드 방식 | 시작 시 1회 로드 (핫 리로드 없음) |
| 환경변수 | **완전 제거** — config.toml이 단일 권위 |
| 파일 없을 때 | 전부 기본값 (zero-config 동작) |

### Config 구조

```toml
[daemon]
idle_timeout = "30m"
socket_path = ""                    # 비우면 플랫폼 기본값

[store]
db_path = ""                        # 비우면 기본값

[queue]
stale_timeout = "1h"
poll_interval = "1s"

[log]
level = "debug"                     # 기본 DEBUG (부하 적으므로 최대한 세밀하게)
file = "apex-agent.log"
max_size_mb = 50
max_backups = 3
audit = true                        # 감사 로그 기본 ON

[build]
command = "cmd.exe /c build.bat"
presets = ["debug", "release"]
```

### 구현 파일

```
internal/config/
├── config.go       # Config 구조체, Load(), DefaultPath(), WriteDefault()
└── config_test.go  # 기본값 테스트, 파일 로드 테스트, 부분 오버라이드 테스트
```

### CLI 커맨드

- `apex-agent config init` — 주석 달린 기본 config.toml 생성
- `apex-agent config show` — 현재 적용 중인 설정 출력

### 기존 코드 영향

- `daemon_cmd.go`: `config.Load()` → 각 모듈에 설정 전달
- `platform/env.go`: 기본값 제공자로 역할 축소 (config가 오버라이드)
- 모든 `APEX_*` 환경변수 참조 제거 (OS 표준 환경변수 `LOCALAPPDATA`, `XDG_DATA_HOME` 등은 유지 — platform 기본값 결정에 필요)

### Config → daemon.Config 매핑

| TOML | daemon.Config 필드 | 기본값 소스 |
|------|-------------------|------------|
| `daemon.idle_timeout` | `IdleTimeout` | 30m |
| `daemon.socket_path` | `SocketAddr` | `platform.SocketPath()` |
| `store.db_path` | `DBPath` | `platform.DBPath()` |
| (자동) | `PIDFilePath` | `platform.PIDFilePath()` |
| `log.*` | 별도 `LogConfig` | 스펙 기본값 |
| `queue.*` | 모듈에 직접 전달 | 스펙 기본값 |
| `build.*` | CLI에서 사용 | 스펙 기본값 |

`daemon.Config`를 확장하거나, 최상위 `config.Config`에서 각 대상(daemon, log, queue, build)으로 분배하는 변환 함수를 `config` 패키지에 둔다.

### 기존 코드 영향 (로깅 포함)

- `daemon_cmd.go`: `config.Load()` → `daemon.Config` 변환 + `slog.SetDefault()` 호출
- `platform/env.go`: 기본값 제공자로 역할 축소 (config가 오버라이드)
- `daemon.go`: `log.New()` 인스턴스 제거 → 글로벌 `log.WithModule()` 사용
- `router.go`, 각 모듈: 인스턴스 기반 logger → 패키지 래퍼 함수 전환

---

## 4. 시스템 바이너리 설치 + 버전 프로토콜

### 설치 경로

```
Windows: %LOCALAPPDATA%/apex-agent/apex-agent.exe
Linux:   ~/.local/bin/apex-agent
```

### settings.json

```json
"command": "apex-agent hook validate-build"
```

상대 경로(`./apex_tools/...`) → 시스템 PATH 기반으로 변경.

### Makefile install

```makefile
VERSION = $(shell git describe --tags --always --dirty)

# 플랫폼별 설치 경로 자동 결정
ifeq ($(OS),Windows_NT)
    INSTALL_DIR ?= $(LOCALAPPDATA)/apex-agent
    EXE = .exe
else
    INSTALL_DIR ?= $(HOME)/.local/bin
    EXE =
endif

install:
    go build -ldflags="-X main.Version=$(VERSION)" \
        -o $(INSTALL_DIR)/apex-agent$(EXE) ./cmd/apex-agent
```

### post-merge git hook

```bash
# apex_tools/git-hooks/post-merge
changed=$(git diff HEAD@{1} --name-only | grep "apex_tools/apex-agent/")
if [ -n "$changed" ]; then
    echo "[apex-agent] 소스 변경 감지, 자동 빌드+설치 중..."
    cd apex_tools/apex-agent && make install
fi
```

`git pull` → apex-agent 소스 변경 시 자동 빌드+설치. 수동 작업 0.

### 버전 프로토콜

```
빌드 시: -ldflags="-X main.Version=v0.1.0-abc1234"

CLI 요청 흐름:
  1. Dial 데몬
  2. daemon.version 요청
  3. 버전 비교
     ├─ 일치 → 정상 진행
     └─ 불일치 → shutdown → 자동 재시작 (새 바이너리) → 원래 요청 재전송
```

DB 스키마는 Migrator가 자동 업그레이드 → 바이너리 버전이 올라가도 DB 호환성 보장.

### 운영 모델

- apex-agent는 인프라 — main에서만 배포
- 일반 feature 브랜치는 apex-agent 소스를 건드리지 않음
- apex-agent 변경 시 전용 브랜치에서 개발 → main 머지 → `make install`
- post-merge hook이 `git pull` 시 자동 처리

---

## 5. 로깅 시스템

### 결정 사항

| 항목 | 결정 |
|------|------|
| 기본 레벨 | **DEBUG** (로컬 도구, 부하 적음, 세밀하게) |
| 감사 로그 | **항상 ON** |
| 출력 | stderr (항상) + 파일 (config.toml 설정 시) |
| 로테이션 | `lumberjack` (max_size_mb + max_backups) |
| 패턴 | **글로벌 + With** (Go 표준, slog.SetDefault) |

### API — 패키지 래퍼 함수

```go
// internal/log/log.go

// 글로벌 래퍼 (spdlog 매크로와 동일한 사용감)
func Debug(msg string, args ...any)
func Info(msg string, args ...any)
func Warn(msg string, args ...any)
func Error(msg string, args ...any)
func Audit(msg string, args ...any)  // level=INFO + audit=true 태그

// 모듈별 컨텍스트 로거
func WithModule(name string) *ModuleLogger

type ModuleLogger struct { ... }
func (ml *ModuleLogger) Debug(msg string, args ...any)
func (ml *ModuleLogger) Info(msg string, args ...any)
func (ml *ModuleLogger) Warn(msg string, args ...any)
func (ml *ModuleLogger) Error(msg string, args ...any)
func (ml *ModuleLogger) Audit(msg string, args ...any)
```

### 사용 예

```go
import "github.com/.../internal/log"

// 모듈에서
var ml = log.WithModule("handoff")

ml.Debug("processing request", "action", "notify-start", "workspace", ws)
ml.Info("state transition", "from", old, "to", new)
ml.Audit("branch registered", "branch", br, "backlog_id", id)
```

### 로그 출력 포맷

```
time=2026-03-22T14:35:42+09:00 level=DEBUG module=handoff msg="processing request" action=notify-start workspace=branch_02
time=2026-03-22T14:35:42+09:00 level=INFO  module=queue msg="lock acquired" channel=build branch=branch_02 pid=1234
time=2026-03-22T14:35:42+09:00 level=INFO  module=handoff audit=true msg="state transition" from=started to=implementing
```

### 로그 레벨 가이드

| 레벨 | 기준 | 예시 |
|------|------|------|
| DEBUG | 모든 핸들러 진입/퇴출, 파라미터, SQL | "dispatching request", "query returned 3 rows" |
| INFO | 상태 변경, 잠금, 감사 이벤트 | "daemon started", "lock acquired" |
| WARN | 비정상이지만 복구 가능 | "stale PID detected", "config file missing" |
| ERROR | 실패, 즉시 조치 필요 | "DB open failed", "IPC write error" |

### 모든 모듈에 로그 추가 대상

- daemon: 시작/종료, 모듈 등록, idle timeout
- ipc: 요청 수신/응답 발송, 에러
- router: 디스패치 진입/완료, 알 수 없는 모듈/액션
- store: DB 열기/닫기, 마이그레이션 실행
- handoff: 모든 상태 전이, 알림 발행/수신, ACK, 게이트 차단/허용
- backlog: CRUD, export, import
- queue: acquire/release, stale 감지, FIFO 순서
- hook: 차단/허용 판정, 입력 파싱
- cleanup: 감지/삭제 대상, dry-run/execute
- context: 컨텍스트 수집 항목
- plugin: 등록/갱신/스킵

### 의존성

`gopkg.in/natefinch/lumberjack.v2` — 로그 로테이션.

---

## 6. E2E 통합 테스트

### 아키텍처

**완전 격리된 임시 환경**에서 실행. 시스템 DB/config에 영향 없음.

```go
// e2e/testenv/env.go

type TestEnv struct {
    Dir        string          // 임시 디렉토리
    ConfigPath string          // 임시 config.toml
    DBPath     string          // 임시 DB
    SocketAddr string          // 고유 소켓 (테스트 이름 기반)
    GitRepo    string          // 임시 git 레포
    Daemon     *daemon.Daemon
    Client     *ipc.Client
}

func New(t *testing.T) *TestEnv   // 환경 생성 + 데몬 시작
func (e *TestEnv) InitGitRepo()   // 임시 git 레포 생성
```

### 실행 방식

빌드 태그 없음 — `go test`에 항상 포함. 기존 `e2e/e2e_test.go`의 `//go:build e2e` 태그도 제거.

```bash
go test ./e2e/...           # E2E만 실행 (로컬 개발 시)
go test ./e2e/... -short    # 스트레스 테스트 스킵 (빠른 확인)
go test ./...               # 단위 + E2E 전부 (CI + 로컬)
```

CI 파이프라인: 기존 `go test ./... -race -cover -v` 스텝에 E2E가 자동 포함. 별도 스텝 불필요.
데몬은 TestEnv가 in-process로 기동하므로 외부 프로세스 의존 없음.

**프로세스 재시작 테스트** (#13 StalePID, #15 DBRecreate): TestEnv의 데몬을 Stop → 환경 조작 → 새 데몬 Start로 시뮬레이션. subprocess spawn 불필요.

### 18개 시나리오

```
e2e/
├── testenv/
│   └── env.go                # 테스트 환경 팩토리
├── daemon_test.go            # 그룹 A: 인프라 (#1)
├── hook_test.go              # 그룹 B: Hook (#2, #3, #4)
├── handoff_test.go           # 그룹 C: 핸드오프 (#5, #6, #7)
├── backlog_test.go           # 그룹 D: 백로그 (#8, #9, #10)
├── queue_test.go             # 그룹 E: 큐 (#11, #12)
├── git_test.go               # 그룹 F: Git (#13, #14)
├── session_test.go           # 그룹 G: 세션 (#15, #16)
└── resilience_test.go        # 그룹 H: 내성 (#17, #18, #19, #20)
```

### 시나리오 상세

**그룹 A: 인프라**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 1 | Daemon_StartIPCRoundtripIdleShutdown | 데몬 시작 → IPC 왕복 → idle timeout → 자동 종료 |

**그룹 B: Hook 파이프라인**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 2 | Hook_ValidateBuildBlocksAndAllows | 빌드 도구 차단 + 허용 명령 통과 |
| 3 | Hook_ValidateMergeRequiresLock | 잠금 없이 머지 차단, 잠금 후 허용 |
| 4 | Hook_MalformedInput | 깨진 JSON, 빈 stdin → 크래시 없음 |

**그룹 C: 핸드오프**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 5 | Handoff_FullLifecycle | start → design → plan → merge 전체 경로 |
| 6 | Handoff_GateEnforcement | 미등록 커밋 차단, status별 소스 차단, 미ack 머지 차단 |
| 7 | Handoff_MultiWorkspace | 두 브랜치 동시 등록 + 양방향 알림 교환 + ack |

**그룹 D: 백로그**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 8 | Backlog_CRUDAndExport | add → list → resolve → export 포맷 검증 |
| 9 | Backlog_MigrationRoundtrip | BACKLOG.md 파싱 → import → export → 구조 비교 |
| 10 | Backlog_RoundtripFidelity | 수동 마크다운 → import → export → diff 0 |

**그룹 E: 큐**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 11 | Queue_AcquireReleaseSerialize | acquire → status → release → 재acquire |
| 12 | Queue_ConcurrentStress | 10 goroutine 동시 요청 → 전부 순차 처리 + 정상 응답 (`-short` 스킵) |

**그룹 F: Git 연동**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 13 | EnforceRebase_AutoAndConflict | behind → 자동 rebase 성공 / 충돌 → 차단+abort |
| 14 | Cleanup_MergedBranchDetection | 머지된 브랜치 감지 + dry-run 확인 |

**그룹 G: 세션**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 15 | Context_OutputFormat | 필수 섹션 포함 (Git Status, Handoff Storage) |
| 16 | Plugin_IdempotentSetup | 2회 setup → 두 번째 변경 없음 |

**그룹 H: 내성**
| # | 테스트 | 검증 내용 |
|---|--------|----------|
| 17 | Resilience_MalformedIPC | 잘못된 IPC 메시지 → 데몬 크래시 없이 에러 응답 |
| 18 | Resilience_DBAutoRecreate | DB 삭제 → 재시작 → 자동 재생성 + 마이그레이션 |
| 19 | Resilience_StalePIDRecovery | 죽은 PID 파일 → CLI 호출 → 자동 재시작 |
| 20 | Resilience_CustomConfigPaths | 비표준 config 경로 → 전체 워크플로우 동작 |

---

## 7. 참조

- 설계 스펙: `docs/apex_tools/plans/20260322_100656_apex_agent_design_spec.md`
- Phase 0 구현 계획: `docs/apex_tools/plans/20260322_104214_apex_agent_phase0_plan.md`
- bash 한계 조사 결과: 12개 항목 (FIXED 2, IMPOSSIBLE 4, IMPRACTICAL 3, NOW POSSIBLE 3)
