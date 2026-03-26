# 워크스페이스 + 세션 관리 모듈 설계서

> **백로그**: BACKLOG-238 (워크스페이스+세션), BACKLOG-239 (백로그 FIX)
> **브랜치**: feature/workspace-session-mgmt
> **작성**: 2026-03-26

## 1. 개요

apex-agent에 두 가지 핵심 기능을 추가한다:

1. **워크스페이스 관리** — 멀티 브랜치 디렉토리 스캔/등록/동기화 + 브랜치 관리 대시보드 페이지
2. **세션 관리** — Claude Code 세션을 ConPTY 기반 독립 프로세스로 제어 + xterm.js 웹 터미널
3. **백로그 FIX** — 대시보드에서 백로그 FIX 트리거 + blocked_reason 알림 시스템

### 1.1 핵심 설계 결정

| 결정 | 선택 | 이유 |
|------|------|------|
| 세션 통신 | ConPTY + WebSocket | 실시간 양방향, xterm.js로 진짜 터미널 경험 |
| 프로세스 배치 | daemon과 분리된 독립 프로세스 | daemon 재시작 시 세션 유지 |
| 프롬프트 전달 | ConPTY stdin 직접 주입 | 큐/락 불필요, Claude Code가 자체 타이밍 관리 |
| DISCUSS 상태 | blocked_reason nullable 필드 | 상태 머신 변경 0, 기존 로직 영향 0 |
| 대시보드 통합 | :7600이 :7601을 리버스 프록시 | 사용자는 단일 URL만 접근 |

## 2. 아키텍처

### 2.1 프로세스 구조

```
┌─ apex-agent daemon (:7600) ────────────────────────────┐
│                                                         │
│  기존 모듈 (backlog, handoff, queue, hook)               │
│  신규 모듈 (workspace)                                   │
│  httpd (대시보드 + 리버스 프록시)                          │
│       /api/session/* ──→ proxy ──→ :7601                │
│       /ws/session/*  ──→ proxy ──→ :7601                │
│                                                         │
│  PID: apex-agent.pid                                    │
│  DB:  apex-agent.db (공유, SQLite WAL)                   │
└─────────────────────────────────────────────────────────┘

┌─ apex-agent session (:7601) ────────────────────────────┐
│                                                         │
│  ConPTY 매니저 (세션별 가상 터미널)                        │
│  WebSocket 서버 (xterm.js ↔ ConPTY I/O)                 │
│  Watchdog (프로세스 사망 감지)                             │
│  세션 REST API                                           │
│                                                         │
│  PID: apex-session.pid                                  │
│  DB:  apex-agent.db (공유)                               │
└─────────────────────────────────────────────────────────┘
```

- **같은 Go 바이너리, 별도 프로세스**: `apex-agent daemon run` / `apex-agent session run`
- **독립 수명**: daemon 재시작해도 session 무관, vice versa
- **DB 공유**: SQLite WAL 모드가 동시 읽기/쓰기 허용
- session 재시작 시 `--resume`으로 Claude Code 대화 컨텍스트 복구 가능

### 2.2 수명 분리 시나리오

**apex-agent 코드 수정 후 재빌드:**
```
1. apex-agent daemon stop     ← 데몬만 종료
2. build.bat                  ← 바이너리 빌드
3. apex-agent daemon start    ← 데몬만 재시작
→ session 프로세스: 영향 0, ConPTY + Claude Code 계속 실행
→ 대시보드 페이지: 잠깐 끊겼다가 HTMX 재연결
→ xterm.js WebSocket: :7601에 직접 연결이라 끊김 없음 (프록시 경유 시 재연결)
```

**session 코드 수정 후 재시작:**
```
1. apex-agent session stop    ← session만 종료 → ConPTY 소멸 → claude 종료
2. apex-agent session start   ← 재시작 → --resume으로 세션 복구
→ daemon: 영향 0
→ 끊김 시간: 수 초, 대화 컨텍스트 보존
```

## 3. DB 스키마

### 3.1 신규 테이블: `local_branches`

```sql
-- workspace v1 마이그레이션
CREATE TABLE local_branches (
    workspace_id    TEXT PRIMARY KEY,
    directory       TEXT NOT NULL UNIQUE,
    git_branch      TEXT,
    git_status      TEXT DEFAULT 'UNKNOWN',   -- CLEAN / DIRTY / UNKNOWN
    session_id      TEXT,                     -- 마지막 세션 ID (resume용)
    session_pid     INTEGER DEFAULT 0,        -- 0 = 없음
    session_status  TEXT DEFAULT 'STOP',      -- STOP / MANAGED / EXTERNAL
    session_log     TEXT,                     -- 현재 세션 로그 파일 경로
    last_scanned    TEXT,
    created_at      TEXT DEFAULT (datetime('now','localtime'))
);
```

**기존 `active_branches`와의 관계:**
```
local_branches (디스크에 존재하는 모든 워크스페이스)
     │
     └─ LEFT JOIN active_branches ON workspace_id = branch
        (핸드오프 등록된 것만 연결)
```

- `local_branches`: 물리 환경 (디스크, 세션)
- `active_branches`: 논리 상태 (핸드오프)
- 핸드오프 없는 브랜치(main)도 세션 관리 가능

### 3.2 기존 테이블 확장: `backlog_items`

```sql
-- backlog v4 마이그레이션
ALTER TABLE backlog_items ADD COLUMN blocked_reason TEXT;
-- NULL = 정상 진행, 값 있으면 유저 결정 대기
```

### 3.3 세션 상태 값

| 값 | 의미 | 대시보드 동작 |
|---|------|-------------|
| `STOP` | 세션 없음 | [새 세션] [이어가기] 버튼 |
| `MANAGED` | session 프로세스가 ConPTY로 관리 | xterm.js 터미널 표시, 프롬프트 입력 가능 |
| `EXTERNAL` | CLI에서 직접 열림 | 읽기 전용 (세션 로그 파싱), 프롬프트 불가 |

**EXTERNAL 감지**: Claude Code SessionStart hook이 apex-agent에 workspace_id + PID를 전달.
해당 PID가 session 프로세스의 managed 세션이 아니면 EXTERNAL로 등록.

## 4. Config 확장

```toml
[workspace]
root = "D:/.workspace"               # 워크스페이스 루트 경로
repo_name = "apex_pipeline"          # 스캔 대상 디렉토리 접두어
scan_on_start = true                 # 데몬 시작 시 자동 스캔

[session]
enabled = true
addr = "localhost:7601"              # session 서버 바인딩
log_dir = ""                         # 기본: $LOCALAPPDATA/apex-agent/sessions/
watchdog_interval = "1s"             # 상태 감지 주기
output_buffer_lines = 500            # WebSocket 재연결 시 리플레이
```

`WriteDefault()`와 `Defaults()`에 동시 반영.

## 5. 워크스페이스 모듈 (`workspace`)

### 5.1 스캔 로직

```
workspace.root 경로에서 디렉토리 순회
  → 이름이 repo_name으로 시작하는 디렉토리 필터
  → 각 디렉토리에서:
      git branch --show-current
      git status --porcelain (비어있으면 CLEAN, 아니면 DIRTY)
  → local_branches에 UPSERT
  → DB에 있지만 디스크에 없는 항목 제거
```

실행 시점: 데몬 OnStart (scan_on_start=true일 때) + 수동 API 트리거.

### 5.2 동기화

- 단일: `git fetch origin main && git pull origin main` (main 브랜치에서만)
- 전체: 모든 main 브랜치 순차 동기화
- 대시보드 [동기화] / [전체 동기화] 버튼으로 트리거

### 5.3 IPC 액션

| 액션 | 설명 |
|------|------|
| `scan` | 수동 스캔 트리거 |
| `list` | 전체 로컬 브랜치 목록 (+ active_branches JOIN) |
| `get` | 단일 브랜치 상세 |
| `sync` | 특정 브랜치 또는 전체 git pull |

## 6. 세션 관리 프로세스 (`session`)

### 6.1 세션 시작

```
POST /api/session/{workspace_id}/start?mode=new|resume

mode=new:
  claude --dangerously-skip-permissions
  (새 세션 ID 생성, DB에 저장)

mode=resume:
  claude --resume {last_session_id} --dangerously-skip-permissions
  (DB에 저장된 마지막 session_id 사용)

공통:
  → ConPTY 생성
  → claude 프로세스를 ConPTY에 attach
  → local_branches UPDATE: session_pid, session_id, session_status='MANAGED'
  → WebSocket 엔드포인트 활성화
```

### 6.2 xterm.js 연결

```
브라우저 → localhost:7600/branches/{id}/terminal (HTML 페이지)
         → ws://localhost:7600/ws/session/{id}
           → httpd 프록시 → ws://localhost:7601/ws/session/{id}
           → ConPTY stdout 실시간 스트리밍
           → 키보드 입력 → ConPTY stdin 직접 주입
```

- 다중 접속 지원 (같은 세션에 여러 브라우저 탭 → broadcast)
- 재연결 시 output_buffer_lines만큼 리플레이
- 프롬프트 전송에 큐/락 없음 — ConPTY stdin에 직접 쓰기, Claude Code가 자체 타이밍 관리

### 6.3 이중 기록 (Tee)

```
ConPTY stdout
    ├──→ WebSocket broadcast (실시간, 주 채널)
    └──→ 파일 로그 (영구 보존)
         sessions/{workspace_id}/{timestamp}.log
```

- 파일 로그는 추가 토큰 소비 없음 (이미 생성된 출력의 로컬 복사)
- WebSocket 끊김 시 파일에서 갭 복구
- 프로세스 사망 시 마지막 출력까지 파일에 보존

### 6.4 Watchdog

```
매 1초 (설정 가능):
  session_pid > 0 인 세션 순회
    → os.FindProcess로 프로세스 존재 확인
    → 사망 → session_status='STOP', session_pid=0
```

### 6.5 CLI

```bash
apex-agent session run              # 포그라운드 (디버깅)
apex-agent session start            # 백그라운드 시작
apex-agent session stop             # 종료
apex-agent session status           # 전체 세션 상태
apex-agent session send <id> "텍스트"  # ConPTY stdin 주입
```

### 6.6 Graceful 재시작

```
session stop:
  → 각 ConPTY에 graceful 종료 신호
  → claude 프로세스 종료 대기 (5초 timeout → kill)
  → session_status='STOP'
  → PID 파일 삭제

session start:
  → 비정상 종료된 세션(MANAGED인데 PID 사망) 리셋
```

## 7. 대시보드 확장

### 7.1 신규 페이지: `/branches`

```
┌─ apex-agent ── [Dashboard] [Branches] [Backlog] [Handoff] [Queue] ─ ⚠ 1 ─┐
│                                                                            │
│  Branches                                              [전체 동기화]       │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ branch_agent    D:/.workspace/apex_pipeline_branch_agent             │  │
│  │ main (CLEAN)    ── MANAGED ──────────────────────── [동기화] [종료]  │  │
│  │ ┌────────────────────────────────────────────────────────────────┐   │  │
│  │ │ (xterm.js 터미널 — 접기/펼치기)                                 │   │  │
│  │ └────────────────────────────────────────────────────────────────┘   │  │
│  ├──────────────────────────────────────────────────────────────────────┤  │
│  │ branch_02       D:/.workspace/apex_pipeline_branch_02                │  │
│  │ feature/auth    ── STOP ── BACKLOG-146 ⚠ "bcrypt 정책 택1"         │  │
│  │                           IMPLEMENTING                               │  │
│  │                    [새 세션] [이어가기] [동기화]                       │  │
│  ├──────────────────────────────────────────────────────────────────────┤  │
│  │ branch_03       D:/.workspace/apex_pipeline_branch_03                │  │
│  │ main (CLEAN)    ── EXTERNAL ── ℹ 터미널 세션                        │  │
│  │                    [읽기 전용 보기] [managed 전환]                     │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────────┘
```

**행별 정보**: workspace_id, 절대 경로, git 브랜치/상태, 핸드오프 상태 (LEFT JOIN), 세션 상태, blocked 백로그.

### 7.2 대시보드 메인 (`/`) 확장

```
Summary 카드 추가:
  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌─────────────────┐
  │ Active 3 │ │ Queue 0  │ │ Branches │ │ ⚠ 결정 대기 1건  │
  │          │ │          │ │ 5 total  │ │                  │
  │          │ │          │ │ 2 active │ │                  │
  └──────────┘ └──────────┘ └──────────┘ └─────────────────┘
```

### 7.3 백로그 페이지 (`/backlog`) 확장

- blocked_reason이 있는 행 강조 + 사유 표시
- 행별 [FIX] 버튼
- 체크박스 다중 선택 + [선택 항목 FIX] 버튼

### 7.4 글로벌 알림 (네비 바 ⚠ 뱃지)

```sql
SELECT COUNT(*) FROM backlog_items
WHERE status='FIXING' AND blocked_reason IS NOT NULL
```

HTMX 1초 폴링. 클릭 시 `/backlog?filter=blocked` 이동.

### 7.5 리버스 프록시

대시보드 httpd(:7600)가 session 서버(:7601)를 프록시:
- `/api/session/*` → HTTP 프록시
- `/ws/session/*` → WebSocket 프록시
- Go `httputil.ReverseProxy` 사용

사용자는 `localhost:7600`만 알면 됨.

## 8. 백로그 FIX 플로우

### 8.1 단일 FIX

```
/backlog [FIX] 클릭
  → 브랜치 선택 모달:
      ○ branch_02 [feature/auth] MANAGED
      ○ branch_03 [main] STOP → 새 세션 시작
      ○ branch_04 [main] EXTERNAL (선택 불가)
  → [FIX 시작] 클릭
      STOP → 세션 자동 시작 (mode=new)
      MANAGED → 기존 세션 사용
  → ConPTY stdin에 프롬프트 주입:
      "BACKLOG-146 즉시 착수하자.\n"
  → /branches 페이지로 이동 (해당 브랜치 터미널 펼침)
```

### 8.2 다중 FIX

체크박스로 여러 항목 선택 → [선택 항목 FIX] → 동일 모달 → 프롬프트:
```
"BACKLOG-146, BACKLOG-179 즉시 착수하자.\n"
```

### 8.3 blocked_reason 플로우

```
에이전트가 설계 결정 필요 발견
  → backlog update 146 --blocked "bcrypt: hard timeout vs sliding 중 택1"
  → blocked_reason 기록, status는 FIXING 유지

대시보드 반응:
  → 네비 바 ⚠ 카운트 증가
  → /backlog 행 강조 + 사유
  → /branches 해당 브랜치에 ⚠

사용자가 ⚠ 클릭 → 해당 브랜치 터미널로 이동 → 지시 입력

에이전트가 결정 수행 후:
  → backlog update 146 --blocked ""
  → ⚠ 해제
```

### 8.4 CLI 확장

```bash
apex-agent backlog update 146 --blocked "사유"    # 설정
apex-agent backlog update 146 --blocked ""         # 해제
```

## 9. REST API 설계서

### 9.1 대시보드 서버 (:7600)

#### 페이지

| Method | Path | 설명 |
|--------|------|------|
| GET | `/` | 대시보드 메인 |
| GET | `/branches` | **신규** — 브랜치 관리 |
| GET | `/backlog` | 백로그 테이블 |
| GET | `/handoff` | 핸드오프 상태 |
| GET | `/queue` | 큐 히스토리 |

#### HTMX 파셜

| Method | Path | 설명 | 비고 |
|--------|------|------|------|
| GET | `/partials/summary` | 요약 카드 | 기존 |
| GET | `/partials/active-branches` | 활성 브랜치 | 기존 |
| GET | `/partials/queue-status` | 큐 상태 | 기존 |
| GET | `/partials/recent-history` | 최근 이벤트 | 기존 |
| GET | `/partials/backlog-table` | 백로그 테이블 | 기존 |
| GET | `/partials/queue-history` | 큐 히스토리 | 기존 |
| GET | `/partials/backlog-inline/{id}` | 백로그 인라인 | 기존 |
| GET | `/partials/branches` | 브랜치 목록 | **신규** |
| GET | `/partials/branch-summary` | Branches 카드 | **신규** |
| GET | `/partials/blocked-badge` | ⚠ 뱃지 카운트 | **신규** |

#### JSON API — 기존

| Method | Path | 설명 |
|--------|------|------|
| GET | `/api/backlog` | 백로그 조회 |
| GET | `/api/handoff` | 핸드오프 조회 |
| GET | `/api/queue` | 큐 조회 |

#### JSON API — Workspace (신규)

| Method | Path | 설명 |
|--------|------|------|
| GET | `/api/workspace` | 전체 브랜치 목록 |
| GET | `/api/workspace/{id}` | 단일 브랜치 상세 |
| POST | `/api/workspace/scan` | 수동 스캔 |
| POST | `/api/workspace/{id}/sync` | 단일 동기화 |
| POST | `/api/workspace/sync-all` | 전체 동기화 |

#### JSON API — Backlog FIX (신규)

| Method | Path | 설명 |
|--------|------|------|
| POST | `/api/backlog/fix` | FIX 트리거 |

```
요청: { backlog_ids: [146], workspace_id: "branch_02" }
응답: { ok: true, prompt_sent: "BACKLOG-146 즉시 착수하자.", session_started: false }
```

#### Session 프록시 → :7601

| Method | Path | 프록시 대상 |
|--------|------|-----------|
| POST | `/api/session/{id}/start` | `:7601/api/session/{id}/start` |
| POST | `/api/session/{id}/stop` | `:7601/api/session/{id}/stop` |
| GET | `/api/session/{id}/status` | `:7601/api/session/{id}/status` |
| POST | `/api/session/{id}/send` | `:7601/api/session/{id}/send` |
| WS | `/ws/session/{id}` | `:7601/ws/session/{id}` |

### 9.2 세션 서버 (:7601)

> 사용자가 직접 접근하지 않음. :7600이 프록시.

| Method | Path | 설명 |
|--------|------|------|
| GET | `/api/sessions` | 전체 세션 목록 |
| GET | `/api/session/{id}/status` | 세션 상태 |
| POST | `/api/session/{id}/start` | 세션 시작 (`?mode=new\|resume`) |
| POST | `/api/session/{id}/stop` | 세션 종료 |
| POST | `/api/session/{id}/send` | stdin 주입 (`{ text: "..." }`) |
| WS | `/ws/session/{id}` | 터미널 스트림 (양방향 바이너리) |

#### WebSocket 프로토콜

```
클라이언트 → 서버: 바이너리 프레임 (키보드 입력 바이트)
서버 → 클라이언트: 바이너리 프레임 (터미널 출력 바이트)
연결 시: output_buffer_lines만큼 최근 출력 리플레이
끊김 시: 자동 재연결 + 갭 리플레이
```

#### 응답 타입

```
Branch {
  workspace_id    string
  directory       string
  git_branch      string
  git_status      string     // CLEAN | DIRTY | UNKNOWN
  session_status  string     // STOP | MANAGED | EXTERNAL
  session_id?     string
  handoff_status? string     // IMPLEMENTING | null
  backlog_ids?    []int
  blocked_backlogs? []BlockedBacklog
}

BlockedBacklog {
  id              int
  title           string
  blocked_reason  string
}

Session {
  workspace_id    string
  status          string     // STOP | MANAGED
  session_id?     string
  pid?            int
  started_at?     string
  log_path?       string
}
```

## 10. 구현 시 검증 필요 사항

| 항목 | 설명 | 검증 방법 |
|------|------|----------|
| ConPTY Go 라이브러리 | Windows ConPTY를 Go에서 다루는 라이브러리 안정성 | PoC: `conpty` 패키지로 echo 프로세스 제어 |
| Claude Code 세션 로그 경로 | `~/.claude/` 아래 정확한 파일 구조와 포맷 | EXTERNAL 세션 읽기 전용 표시에 필요 |
| `--resume` 동작 | 세션 ID 지정 시 대화 복구 범위와 제약 | session restart 시나리오 테스트 |
| WebSocket 프록시 | `httputil.ReverseProxy`의 WebSocket 지원 범위 | 양방향 바이너리 프레임 전달 검증 |
| SQLite 동시 접근 | daemon과 session 두 프로세스의 WAL 모드 동시 쓰기 | 스트레스 테스트 |

## 11. 향후 확장

- BACKLOG-240: 대시보드 React 리팩토링 — HTMX/Go 템플릿에서 React SPA로 전환
- BACKLOG-187: HTTP 대시보드 워크플로우 트리거 UI — auto-review / FSD 백로그 소탕 버튼 (본 설계의 FIX 기능과 통합 가능)
