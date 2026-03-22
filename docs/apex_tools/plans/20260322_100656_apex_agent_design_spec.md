# apex-agent: 개발 자동화 플랫폼 설계 스펙

- **상태**: 설계 확정
- **백로그**: BACKLOG-126
- **스코프**: tools, infra
- **작성일**: 2026-03-22
- **선행 문서**: `20260322_015226_apex_agent_go_backend.md` (논의 단계)

---

## 1. 비전

apex-agent는 단순한 bash 스크립트 대체가 아니라, apex_pipeline의 **개발 인프라 플랫폼**.
C++ 코어가 런타임 인프라라면, apex-agent는 **개발 워크플로우의 인프라**.

```
apex_core (C++)              apex-agent (Go)
━━━━━━━━━━━━━━━              ━━━━━━━━━━━━━━━━
런타임 프레임워크              개발 자동화 프레임워크
서비스가 올라탐                모듈이 올라탐
Server → ServiceBase          Daemon → Module interface
코루틴 + Asio                 goroutine + channel
네트워크 I/O                  IPC + SQLite
```

Go를 apex_pipeline 개발 생태계의 인프라 언어로 선택한다.
향후 추가 개발 도구도 이 기반 위에 모듈로 확장한다.

---

## 2. 설계 원칙

코어 프레임워크의 설계 철학을 개발 인프라에 이식한다.

| # | 원칙 | 코어 대응 | apex-agent 적용 |
|---|------|-----------|-----------------|
| 1 | **모듈 독립성** | 서비스는 서로 모른다 | handoff 모듈은 queue 모듈 내부를 모른다 |
| 2 | **코어가 모듈을 호출, 역방향 금지** | Server가 ServiceBase 훅 호출 | Daemon이 Module 인터페이스 호출 |
| 3 | **공유 상태는 코어가 관리** | Server가 session/timer 관리 | Daemon이 SQLite/IPC 관리 |
| 4 | **설정은 선언적** | ServerConfig + YAML | 모듈 등록 = 구조체 반환 |
| 5 | **플랫폼 추상화** | Asio가 OS 추상화 | `internal/platform`이 Windows/Linux 추상화 |

---

## 3. 핵심 결정 사항

브레인스토밍에서 확정된 사항:

| 항목 | 결정 | 근거 |
|------|------|------|
| 언어 | **Go** | DevOps 표준, 콜드 ~10ms, 단일 바이너리, 크로스컴파일 간단 |
| 실행 모델 | **CLI thin client + 싱글톤 데몬** | 중앙 관리, 일관된 진입점, 장애 복구 용이 |
| IPC | **Named Pipe (Win) / Unix Socket (Linux)** | Windows 네이티브, 방화벽 무관, go-winio (MS 공식) |
| 직렬화 | **JSON** | Go 표준 라이브러리, 페이로드 소량, 디버깅 가독성 |
| 상태 저장소 | **SQLite (pure Go, WAL 모드)** | ACID, 동시 접근 직렬화, modernc.org/sqlite (CGo-free) |
| 배포 | **로컬 `go build` + CI Release artifact** | 개발 중 즉시 빌드, CI에서 크로스컴파일 배포 |
| 마이그레이션 | **Foundation-First (Bottom-Up)** | 기반 → 모듈 순서, 코어 프레임워크와 동일 전략 |
| 스코프 | **전면 재작성** | Go가 인프라 언어, 하이브리드 불필요 |
| BACKLOG-50 | **흡수** | 스크립트 정리가 아니라 Go로 대체 |
| 데몬 인스턴스 | **시스템당 1개** | 중앙 관리 시스템, 모든 워크스페이스가 단일 데몬에 연결 |

---

## 4. 계층 아키텍처

3계층 구조: CLI → Daemon Core → Concrete Modules.

```
┌─────────────────────────────────────────────────────┐
│                    CLI (thin client)                  │
│  apex-agent handoff notify start --backlog 126       │
│  apex-agent queue acquire build                      │
│  apex-agent daemon start/stop/status                 │
└──────────────────────┬──────────────────────────────┘
                       │ Named Pipe (Win) / Unix Socket (Linux)
┌──────────────────────▼──────────────────────────────┐
│              Layer 0: Daemon Core                     │
│                                                      │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐ ┌──────┐ │
│  │ IPC      │ │ SQLite   │ │ Platform   │ │ Log  │ │
│  │ Server   │ │ Store    │ │ Abstraction│ │      │ │
│  └──────────┘ └──────────┘ └────────────┘ └──────┘ │
│                                                      │
│  Module Registry: 모듈 발견 → 라우트 등록 → 수명 관리  │
└──────────────────────┬──────────────────────────────┘
                       │ Module Interface
┌──────────────────────▼──────────────────────────────┐
│              Layer 1: Module Interface                │
│                                                      │
│  type Module interface {                             │
│      Name() string                                   │
│      RegisterRoutes(router Router)                   │
│      RegisterSchema(migrator Migrator)               │
│      OnStart(ctx context.Context) error              │
│      OnStop() error                                  │
│  }                                                   │
└──────────────────────┬──────────────────────────────┘
                       │ 구체 구현
┌──────────────────────▼──────────────────────────────┐
│              Layer 2: Concrete Modules                │
│                                                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ │
│  │ Handoff  │ │ Queue    │ │ Hook     │ │Cleanup │ │
│  │ Module   │ │ Module   │ │ Gate     │ │ Module │ │
│  └──────────┘ └──────────┘ └──────────┘ └────────┘ │
│  ┌──────────┐ ┌──────────┐                          │
│  │ Backlog  │ │ Context  │                          │
│  │ Module   │ │ Module   │                          │
│  └──────────┘ └──────────┘                          │
└─────────────────────────────────────────────────────┘
```

### 요청 흐름 예시

`apex-agent handoff notify start --backlog 126`:

1. CLI가 요청을 JSON으로 직렬화
2. Named Pipe로 데몬에 전송
3. 데몬 IPC Server 수신 → Router가 "handoff" 모듈로 라우팅
4. Handoff 모듈이 SQLite 트랜잭션으로 상태 변경
5. 결과를 CLI에 반환 → CLI가 stdout/stderr + exit code로 출력

### Module ↔ ServiceBase 대응

| ServiceBase (C++) | Module (Go) | 역할 |
|---|---|---|
| `on_configure()` | `RegisterSchema()` | 초기화 시 스키마/설정 등록 |
| `on_start()` | `OnStart()` | 모듈 시작 |
| `on_stop()` | `OnStop()` | 모듈 정리 |
| `handle()` / `route()` | `RegisterRoutes()` | 요청 핸들러 등록 |

모듈은 코어가 제공하는 Store, Logger, Platform을 주입받아 사용한다.
모듈 간 직접 import 금지. 모듈 간 통신 필요 시 코어의 이벤트 버스를 통한다.

### 모듈 시작 순서

모듈은 등록 순서대로 `OnStart()` 호출. 현재 모듈 간 의존성이 거의 없으므로 명시적 의존 선언 없이 등록 순서로 충분하다. 향후 모듈 간 이벤트 통신이 필요해지면 코어에 이벤트 버스를 추가한다.

### Context Module

`session-context.sh`를 대체. SessionStart 시 프로젝트 컨텍스트(git 상태, 핸드오프 요약, 최근 커밋)를 수집하여 반환한다. 다른 모듈의 상태를 읽기 전용으로 조회하여 통합 컨텍스트를 생성하는 역할.

---

## 5. 데이터 모델 (SQLite)

### 설계 철학

기존 파일 시스템의 1:1 대응이 아니라, DB의 정합성 보장을 활용하여 **우회 장치를 제거하고 간소화**한다.

| 기존 우회 장치 | 왜 필요했나 | DB에서 | 결과 |
|---|---|---|---|
| `watermarks/` | 파일 전체 재읽기 방지 마킹 | `WHERE id > last_seen` 쿼리 | 제거 |
| `backlog-status/` | 백로그↔브랜치 링크 별도 파일 | branches 테이블에 FK | 제거 |
| `*.lock/` + `*.owner` | mkdir 원자성 트릭 잠금 | queue 테이블 `status='active'` | 흡수 |
| `index` (pipe-delimited) | append-only 동시 쓰기 안전 | INSERT 트랜잭션 | 흡수 |
| `payloads/` 별도 디렉토리 | 로그에 마크다운 불가 | TEXT 컬럼 | 흡수 |

### 스키마 (4개 테이블)

```sql
-- ① 백로그 항목 (BACKLOG.md + BACKLOG_HISTORY.md 대체)
CREATE TABLE backlog_items (
    id          INTEGER PRIMARY KEY,        -- #N 번호
    title       TEXT    NOT NULL,
    severity    TEXT    NOT NULL,            -- CRITICAL | MAJOR | MINOR
    timeframe   TEXT    NOT NULL,            -- NOW | IN_VIEW | DEFERRED
    scope       TEXT    NOT NULL,            -- 쉼표 구분
    type        TEXT    NOT NULL,            -- bug | design-debt | test | docs | perf | security | infra
    description TEXT    NOT NULL,
    related     TEXT,                        -- 쉼표 구분 ID (소규모 데이터셋에서 실용적 트레이드오프, 정규화 비용 불필요)
    position    INTEGER NOT NULL,            -- timeframe 내 정렬 순서
    status      TEXT    NOT NULL DEFAULT 'open',  -- open | resolved
    resolution  TEXT,                        -- FIXED | WONTFIX | DUPLICATE | SUPERSEDED
    resolved_at TEXT,
    created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);

-- ② 브랜치 상태 (handoff_branches + backlog_links 통합)
CREATE TABLE branches (
    branch      TEXT    PRIMARY KEY,
    workspace   TEXT    NOT NULL,
    status      TEXT    NOT NULL,            -- started | design-notified | implementing | merge-notified
    backlog_id  INTEGER REFERENCES backlog_items(id),
    summary     TEXT,
    created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);

-- ③ 알림 (notifications + acks, watermarks 제거)
CREATE TABLE notifications (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    branch      TEXT    NOT NULL,            -- 발신 브랜치
    workspace   TEXT    NOT NULL,
    type        TEXT    NOT NULL,            -- start | design | plan | merge
    summary     TEXT,
    payload     TEXT,                        -- 설계 문서 등
    created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);

CREATE TABLE notification_acks (
    notification_id INTEGER NOT NULL REFERENCES notifications(id),
    branch          TEXT    NOT NULL,        -- 수신 브랜치
    action          TEXT    NOT NULL,        -- no-impact | will-rebase | rebased | design-adjusted | deferred
    acked_at        TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
    PRIMARY KEY (notification_id, branch)
);

-- ④ 큐 (queue_entries + queue_locks 통합)
CREATE TABLE queue (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    channel    TEXT    NOT NULL,             -- build | merge
    branch     TEXT    NOT NULL,
    pid        INTEGER NOT NULL,
    status     TEXT    NOT NULL DEFAULT 'waiting',  -- waiting | active | done
    created_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
);
-- 잠금 = WHERE channel=? AND status='active' (행이 곧 잠금)

-- 인덱스 (구현 시 쿼리 패턴에 따라 추가)
CREATE INDEX idx_notifications_branch ON notifications(branch);
CREATE INDEX idx_acks_branch ON notification_acks(branch);
CREATE INDEX idx_queue_channel_status ON queue(channel, status);
```

### 백로그 관리 흐름

```bash
apex-agent backlog add --title "..." --severity MAJOR --timeframe IN_VIEW
apex-agent backlog list --timeframe NOW --severity CRITICAL
apex-agent backlog check 126
apex-agent backlog resolve 126 --resolution FIXED
apex-agent backlog export > docs/BACKLOG.md
```

BACKLOG.md는 DB에서 생성하는 **읽기 전용 export**로 전환된다.
export 포맷은 기존 `docs/CLAUDE.md` 백로그 템플릿 구조(NOW/IN VIEW/DEFERRED 섹션, 항목 템플릿, `다음 발번` 카운터)를 유지하여 호환성을 보장한다.

### 마이그레이션

- 모듈별 버전 기반 마이그레이션 (`RegisterSchema`로 등록)
- 기존 파일 → DB 초기 전환: `apex-agent migrate` 커맨드로 1회 실행
- 전환 후 파일 데이터는 아카이브 디렉토리로 이동

---

## 6. IPC & 플랫폼

### 트랜스포트 추상화

```go
type Transport interface {
    Listen() (net.Listener, error)
    Dial() (net.Conn, error)
}
```

| 플랫폼 | 트랜스포트 | 구현 |
|---|---|---|
| Windows | Named Pipe `\\.\pipe\apex-agent` | `github.com/Microsoft/go-winio` |
| Linux | Unix Socket `/tmp/apex-agent.sock` | `net.Listen("unix", ...)` |

빌드 태그(`//go:build windows`, `//go:build !windows`)로 플랫폼별 컴파일.
Windows가 1등 시민, Linux(CI)가 2등 시민.

### 플랫폼 추상화 해소 목록

| 영역 | bash 시절 문제 | Go 해소 |
|---|---|---|
| 경로 | MSYS 변환 버그 (#89, #90) | `filepath.Abs()` 네이티브 |
| PID | `kill -0` + `tasklist` 폴백 | `os.FindProcess()` 크로스플랫폼 |
| 잠금 | `mkdir` 원자성 트릭 | DB 트랜잭션 / Named Mutex |
| 프로세스 수명 | 셸 종료 시 같이 죽음 | 독립 데몬 프로세스 |

---

## 7. 데몬 수명 관리

### 싱글톤

시스템당 데몬 1개. 모든 워크스페이스가 동일 데몬에 연결.

```
workspace branch_01 ──┐
                      ├──→ \\.\pipe\apex-agent ──→ [ 데몬 1개 ]
workspace branch_02 ──┤                              │
                      │                         apex-agent.db (단일)
터미널 직접 호출 ─────┘
```

- 파이프 이름: 고정 (`\\.\pipe\apex-agent`)
- DB 위치: `%LOCALAPPDATA%/apex-agent/apex-agent.db`
- PID 파일: `%LOCALAPPDATA%/apex-agent/apex-agent.pid`
- CLI 요청에 workspace 식별자 포함

### 수명 사이클

```
CLI 호출 → Dial() 시도
  ├─ 성공 → 요청 전송 → 응답 수신
  └─ 실패 (데몬 없음)
       1. daemon start (백그라운드 프로세스 spawn)
       2. readiness 폴링: 50ms 간격으로 Dial() 재시도 (최대 3초)
       3. 성공 → 정상 흐름
       4. 3초 초과 → "daemon failed to start" 에러 + exit 1
       5. 데몬 크래시 루프 감지: 10초 내 3회 재시작 실패 → 진단 로그 출력 후 중단

데몬 내부:
  시작: PID 파일 작성 → SQLite 열기 (WAL) → 모듈 등록/시작 → IPC 리스너
  실행: IPC 수신 → Router → Module → 응답 (단일 goroutine이 쓰기 직렬화)
  유휴: 마지막 요청 후 30분 무활동 → 자동 종료 (모든 CLI 요청이 타이머 리셋)
  종료: 신규 수신 중단 → 진행 중 완료 대기 (5초) → 모듈 역순 OnStop → DB 닫기 → PID 삭제

장애 복구: CLI가 stale PID 감지 (파일 있는데 프로세스 없음) → PID 파일 정리 → 재시작
```

### 동시 쓰기 직렬화

데몬은 단일 프로세스이며, SQLite 쓰기 연산은 단일 goroutine(또는 mutex)으로 직렬화한다.
모든 CLI 요청은 데몬을 경유하므로, 다수 워크스페이스의 동시 요청도 데몬 내부에서 자연스럽게 직렬화된다.
SQLite WAL의 단일 쓰기 제약은 데몬 아키텍처에서 비이슈.

---

## 8. 프로젝트 구조

```
apex_tools/apex-agent/
├── cmd/
│   └── apex-agent/
│       └── main.go                 # 엔트리포인트
├── internal/
│   ├── daemon/                     # Layer 0: Daemon Core
│   │   ├── daemon.go               # 수명 관리, 모듈 레지스트리, 시그널
│   │   ├── router.go               # IPC 요청 → 모듈 라우팅
│   │   └── module.go               # Module interface 정의
│   ├── ipc/                        # IPC 전송 계층
│   │   ├── server.go               # 리스너 (데몬 측)
│   │   ├── client.go               # 다이얼러 (CLI 측, 자동 데몬 시작)
│   │   ├── protocol.go             # JSON 요청/응답 프레임
│   │   ├── transport_windows.go    # Named Pipe
│   │   └── transport_unix.go       # Unix socket
│   ├── store/                      # SQLite 인프라
│   │   ├── store.go                # DB 연결, WAL, 트랜잭션 헬퍼
│   │   └── migrator.go             # 모듈별 버전 기반 마이그레이션
│   ├── platform/                   # 크로스플랫폼 추상화
│   │   ├── path.go                 # filepath 유틸
│   │   ├── pid.go                  # 프로세스 생존 체크
│   │   └── env.go                  # 환경 변수, 데이터 디렉토리
│   ├── cli/                        # CLI thin client
│   │   ├── root.go                 # cobra 루트 커맨드
│   │   ├── daemon_cmd.go           # daemon start/stop/status
│   │   ├── handoff_cmd.go          # handoff notify/check/ack
│   │   ├── queue_cmd.go            # queue acquire/release
│   │   ├── backlog_cmd.go          # backlog add/list/resolve/export
│   │   └── migrate_cmd.go          # 파일→DB 마이그레이션
│   ├── log/                        # 구조화 로깅
│   │   └── log.go                  # slog 래퍼
│   └── modules/                    # Layer 2: 구체 모듈
│       ├── handoff/
│       │   ├── module.go           # Module interface 구현
│       │   ├── state.go            # 상태머신
│       │   └── notify.go           # 알림 발행/조회
│       ├── queue/
│       │   ├── module.go
│       │   ├── fifo.go             # FIFO 큐
│       │   └── lock.go             # 채널별 잠금
│       ├── backlog/
│       │   ├── module.go
│       │   ├── manage.go           # CRUD + 순서 관리
│       │   └── export.go           # 마크다운 내보내기
│       ├── hook/
│       │   ├── module.go
│       │   └── gate.go             # validate-build/merge/handoff 통합
│       └── cleanup/
│           └── module.go           # 브랜치 정리
├── go.mod
├── go.sum
├── Makefile                        # build, test, lint, cross-compile
└── .goreleaser.yml                 # CI Release용 (선택)
```

---

## 9. 테스트 전략

3계층 테스트 피라미드:

| 계층 | 대상 | 방식 | 비율 |
|------|------|------|------|
| **Unit** | 상태머신 전이, FIFO 정렬, 백로그 position, export 포맷 | 순수 함수, DB 불필요 | 70% |
| **Integration** | 모듈 + SQLite 상호작용 | `:memory:` DB 주입 | 25% |
| **E2E** | 데몬→CLI 전체 경로 | 임시 데몬+DB spawn | 5% |

핵심 설계:
- 모듈은 `Store` 인터페이스에 의존 → 테스트 시 in-memory DB 주입
- 상태머신은 순수 함수로 분리 → DB 없이 단위 테스트
- CI: `go test ./... -race -cover` (기존 C++ CI와 병렬 실행)

---

## 10. Phase 계획

Foundation-First 원칙. 각 Phase 완료 시 해당 bash 스크립트 즉시 삭제.

| Phase | 내용 | 핵심 산출물 | 삭제 대상 |
|:-----:|------|-----------|----------|
| **0** | 기반 인프라 (daemon, IPC, SQLite, platform, CLI, log, CI) | 데몬 기동 + 빈 모듈 IPC 왕복 | — |
| **1** | Hook Gate 모듈 (validate-build/merge) | 실제 Claude Code hook 동작 | `validate-build.sh`, `validate-merge.sh` |
| **2** | Backlog 모듈 (CRUD, export, migrate) | 백로그 DB 관리, BACKLOG.md export | BACKLOG.md 직접 편집 중단 |
| **3** | Handoff 모듈 (상태머신, 알림, ACK) | 멀티 워크스페이스 핸드오프 | `branch-handoff.sh`, `validate-handoff.sh`, `handoff-probe.sh` |
| **4** | Queue 모듈 (FIFO, 잠금) | 빌드/머지 큐 직렬화 | `queue-lock.sh` |
| **5** | 나머지 (cleanup, context, enforce-rebase) | 모든 bash 제거, Go 단일 운영 | `cleanup-branches.sh`, `session-context.sh`, `enforce-rebase.sh`, `setup-claude-plugin.sh` |

### 스코프 외 스크립트

| 스크립트 | 사유 |
|----------|------|
| `build-preflight.sh` | 빌드 시스템(`build.bat`)이 소스하는 스크립트. hook이 아니라 빌드 파이프라인 소속이므로 apex-agent 스코프 밖. 별도 판단. |

### `setup-claude-plugin.sh` 처리

현재 SessionStart hook으로 auto-review 플러그인을 `~/.claude/plugins/`에 등록하는 역할.
Phase 5에서 Context Module에 흡수: 데몬 시작 시 플러그인 등록 상태를 확인하고 필요 시 갱신.
auto-review 플러그인 자체(Markdown 스킬)는 Claude Code 플러그인이므로 apex-agent 스코프 밖.

Phase 순서 논리:
- Phase 0: 기반 없이 모듈 없음
- Phase 1: 가장 단순 → end-to-end 검증
- Phase 2: 가장 시급한 현실 고통 (백로그 컨플릭트)
- Phase 3: 가장 복잡, 기반 성숙 필요
- Phase 4: Handoff 연동, Phase 3 이후 자연스러움
- Phase 5: 독립적, 순서 유연

---

## 11. 확장성

### 백로그 → JIRA 이관

Backlog 모듈을 제거하면 끝. JIRA 연동은 Claude Code MCP 등 별도 경로.

### 중앙 관리 시스템 승격

데몬 IPC 레이어 위에 HTTP/gRPC 엔드포인트 추가로 원격 접근 가능.
모듈 코드 변경 없음. 트랜스포트 계층만 확장.

```
현재:  CLI → Named Pipe → Daemon → Module
미래:  CLI → Named Pipe → Daemon → Module
       Web → HTTP/gRPC ─────┘
```

### 플러그인 생태계와의 관계

apex-agent는 **개발 인프라 백엔드**이고, Claude Code 플러그인(auto-review 등)은 **에이전트 워크플로우 프론트엔드**.
둘은 별개 계층이며, 플러그인이 apex-agent CLI를 호출하는 관계(플러그인 → CLI → 데몬).
플러그인 등록/관리는 Phase 5에서 Context Module이 흡수.

### 향후 모듈

기반 완성 후 백로그에 추가하여 별도 논의 예정.
Module interface가 안정적이면 새 모듈 추가 비용은 최소.

---

## 12. 참조

- 선행 논의 문서: `docs/apex_tools/plans/20260322_015226_apex_agent_go_backend.md`
- 현재 hook 설정: `.claude/settings.json`
- hook 스크립트: `.claude/hooks/`
- 자동화 도구: `apex_tools/`
- 코어 프레임워크 가이드: `docs/apex_core/apex_core_guide.md`
- 관련 버그: BACKLOG-89 (MSYS 경로), BACKLOG-90 (MSYS 경로)
- 관련 백로그: BACKLOG-50 (스크립트 정리 → 본 작업으로 흡수)
