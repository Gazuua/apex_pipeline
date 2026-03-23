# HTTP 대시보드 + 워크플로우 컨트롤 설계

> BACKLOG-154, BACKLOG-159 | 브랜치: feature/http-dashboard-workflow

## 목적

apex-agent 데몬에 HTTP 서버를 내장하여:
1. **인간(개발자)** — 브라우저에서 시스템 상태를 실시간 시각화하고 워크플로우를 직접 컨트롤
2. **에이전트** — JSON API로 프로그래매틱 접근

## 아키텍처

### HTTP 서버 위치: 데몬 내장

```
Daemon
├── IPC Server  (기존 — Named Pipe/Unix Socket)
├── HTTP Server (신규 — net/http, localhost:7600)
├── Router      (기존 — 모듈 디스패치, 쓰기 시 공유)
├── Store       (기존 — SQLite, 읽기 시 직접 접근)
└── Modules     (기존 — backlog, handoff, queue, hook)
```

- Go 표준 `net/http` — 외부 프레임워크 없음
- `localhost` 바인딩 전용 — 로컬 도구이므로 인증 불필요
- HTTP 요청도 데몬 idle 타이머 리셋 → 대시보드가 열려있는 한 데몬 유지
- **포트 충돌 시**: 경고 로그 출력 + IPC만으로 계속 운영 (데몬 종료하지 않음)
- **보안**: localhost 바인딩이므로 네트워크 외부 접근 불가. CSRF는 로컬 싱글 유저 도구 특성상 수용

### 데이터 접근 패턴: 하이브리드

- **읽기**: HTTP 핸들러 → Store 직접 쿼리 (JOIN/집계 자유)
- **쓰기**: HTTP 핸들러 → Router.Dispatch() → 모듈 핸들러 (검증/상태 전이 보장)

### 프론트엔드: Go 템플릿 + HTMX

- Go `html/template` 서버 사이드 렌더링
- HTMX (14KB) — HTML 속성으로 AJAX 처리, JS 최소화
- `embed.FS`로 static/templates를 바이너리에 내장 → 단일 바이너리 배포
- 빌드 도구(npm, node) 불필요

## 패키지 구조

```
internal/
├── daemon/
│   └── daemon.go          # HTTP 서버 기동 추가
├── httpd/                  # 신규 패키지
│   ├── server.go          # Server 구조체, Start/Stop, 미들웨어
│   ├── routes.go          # 라우트 등록
│   ├── handler_dashboard.go   # 메인 대시보드 뷰
│   ├── handler_backlog.go     # 백로그 뷰
│   ├── handler_handoff.go     # 핸드오프 뷰
│   ├── handler_queue.go       # 큐 뷰
│   ├── static/                # embed.FS로 내장 (httpd 패키지 내 — embed 제약)
│   │   ├── htmx.min.js        # HTMX 2.0.4 (BSD 2-Clause)
│   │   └── style.css
│   └── templates/             # Go html/template
│       ├── layout.html        # 베이스 레이아웃 (nav, footer)
│       ├── dashboard.html     # 메인 대시보드
│       ├── backlog.html       # 백로그 목록
│       ├── handoff.html       # 핸드오프 상태
│       ├── queue.html         # 큐 상태
│       └── partials/          # HTMX 교체용 HTML 조각
```

## 핵심 인터페이스

```go
type Server struct {
    store       *store.Store
    router      *daemon.Router
    httpSrv     *http.Server
    tmpl        *template.Template
    lastRequest atomic.Int64       // idle 타이머 리셋용
}

func New(store *store.Store, router *daemon.Router, addr string) *Server
func (s *Server) Start() error
func (s *Server) Stop() error
func (s *Server) LastRequestTime() int64  // Daemon idle 루프에서 조회
```

### 데몬 통합

```go
// daemon.go Run()
ipcServer.Start()

httpServer := httpd.New(d.store, d.router, cfg.HTTP.Addr)
if err := httpServer.Start(); err != nil {
    log.Warn("HTTP server failed to start, dashboard unavailable", "error", err)
    // IPC만으로 계속 운영 — 데몬 종료하지 않음
}

// idle 체크 시 IPC와 HTTP 양쪽의 마지막 요청 시각 중 최신값 사용
lastActivity := max(ipcServer.LastRequestTime(), httpServer.LastRequestTime())

// Graceful shutdown 순서: HTTP → IPC → Modules (역순)
// HTTP Shutdown 타임아웃: 5초
ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
httpServer.Stop(ctx)
cancel()
ipcServer.Stop()
```

### idle 타이머 미들웨어

```go
func (s *Server) idleResetMiddleware(next http.Handler) http.Handler {
    return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
        s.lastRequest.Store(time.Now().Unix())
        next.ServeHTTP(w, r)
    })
}
```

### workspace 결정 (쓰기 API용)

HTTP 요청에는 IPC의 `Request.Workspace` 개념이 없다. 쓰기 API에서 `Router.Dispatch()` 호출 시:
- 데몬 시작 시 CWD에서 workspace ID를 캡처하여 `Server.defaultWorkspace`에 저장
- HTTP 요청 파라미터 `?workspace=`로 오버라이드 가능 (기본값: 캡처된 workspace)
- 1차 스코프에서는 쓰기 API가 없으므로 구현은 2차로 미뤄도 됨. 인터페이스만 준비

## 페이지 구성

### 메인 대시보드 (`/`)

| 섹션 | 내용 |
|------|------|
| 활성 브랜치 | active_branches — 상태, 연결 백로그 |
| 큐 상태 | build/merge 잠금 현황 |
| 백로그 요약 | OPEN/FIXING/RESOLVED 건수, severity 분포 |
| 최근 이력 | branch_history — 최근 머지/드롭 |

### 백로그 (`/backlog`)

- 필터: status, severity, timeframe, scope, type
- 정렬: ID, severity, 생성일, 수정일
- 클릭 시 상세 보기 (description + 관련 브랜치)
- HTMX 필터 변경 시 테이블만 교체

### 핸드오프 (`/handoff`)

- active_branches + 상태 머신 시각화
- branch_backlogs JOIN으로 연결 백로그 표시
- branch_history 탭

### 큐 (`/queue`)

- 채널별 잠금 상태 (build, merge)
- 대기 중인 요청
- 잠금 이력

## API 엔드포인트

### 페이지 렌더링 (전체 HTML)

| 메서드 | 경로 | 설명 |
|--------|------|------|
| GET | `/` | 메인 대시보드 |
| GET | `/backlog` | 백로그 테이블 |
| GET | `/handoff` | 핸드오프 현황 |
| GET | `/queue` | 큐 상태 |

### HTMX Partials (HTML 조각 — 폴링 대상)

| 메서드 | 경로 | 설명 |
|--------|------|------|
| GET | `/partials/summary` | 대시보드 요약 카드 |
| GET | `/partials/active-branches` | 활성 브랜치 목록 |
| GET | `/partials/queue-status` | 큐 잠금 상태 |
| GET | `/partials/recent-history` | 최근 이력 |
| GET | `/partials/backlog-table` | 필터링된 백로그 (쿼리 파라미터) |
| GET | `/partials/handoff-detail/{id}` | 핸드오프 상세 |

### 액션 API (쓰기 — Router 경유, 2차 스코프)

| 메서드 | 경로 | 설명 |
|--------|------|------|
| POST | `/api/action/review` | auto-review 트리거 |
| POST | `/api/action/fsd-backlog` | fsd-backlog 트리거 |

### JSON API (에이전트/프로그래매틱)

| 메서드 | 경로 | 설명 |
|--------|------|------|
| GET | `/api/backlog` | 백로그 JSON |
| GET | `/api/handoff` | 핸드오프 JSON |
| GET | `/api/queue` | 큐 상태 JSON |

## 실시간 업데이트

- 기본 폴링: **1초** (`hx-trigger="every 1s"`)
- 폴링 속도 조절 UI: Fast (0.5s) / Normal (1s) / Slow (2s)
- 폴링 속도 설정은 `localStorage`에 저장 — 페이지 새로고침 시에도 유지
- SSE/WebSocket 불사용 — 싱글 유저 로컬 도구에 폴링으로 충분
- **폴링 쿼리 경량화 원칙**: 각 partial 쿼리는 단순 SELECT/COUNT 위주. 무거운 JOIN/집계는 피하고, 필요 시 서버 사이드 캐시(짧은 TTL) 적용. SQLite WAL 모드 읽기-쓰기 경합 최소화

```html
<div hx-get="/partials/summary" hx-trigger="every 1s" data-poll>
```

```javascript
function setPollRate(rate) {
    document.querySelectorAll('[data-poll]').forEach(el => {
        el.setAttribute('hx-trigger', 'every ' + rate);
        htmx.process(el);
    });
}
```

## 디자인 시스템

쇼케이스 페이지(`docs/showcase/agent_ecosystem.html`) 다크 테마 계승:

| 토큰 | 값 | 용도 |
|------|-----|------|
| --bg-base | #0F172A | 페이지 배경 |
| --bg-card | #1E293B | 카드/패널 |
| --border | #334155 | 테두리 |
| --text-primary | #E2E8F0 | 본문 |
| --text-secondary | #94A3B8 | 보조 텍스트 |
| --accent-green | #34D399 | 성공, 활성 |
| --accent-blue | #38BDF8 | 정보, 링크 |
| --accent-purple | #A78BFA | 진행중 |
| --accent-amber | #F59E0B | 경고, 대기 |
| --accent-red | #F87171 | 에러, 차단 |
| --font-sans | Segoe UI, system-ui | UI 텍스트 |
| --font-mono | Cascadia Code, Fira Code | 코드/ID |

## 1차 스코프 경계

### 포함

- HTTP 서버 데몬 내장 + config
- 4개 페이지 (대시보드, 백로그, 핸드오프, 큐)
- HTMX partials + 1초 폴링 + 속도 조절
- JSON API 엔드포인트
- 다크 테마 UI
- Go httptest 단위 테스트 + E2E

### 제외 (후순위)

- auto-review/fsd-backlog 트리거 (2차)
- git 서브커맨드 — checkout-main, switch, rebase (2차, #154)
- 프론트엔드 테스트 (Playwright 등)

## 테스트 전략

- Go `httptest` — 핸들러 단위 (응답 코드, HTML 조각 포함 여부)
- E2E — `e2e/testenv` 인프라에 HTTP 서버 기동 추가, 엔드포인트 응답 검증
  - `daemon.Config`에 `HTTP` 필드 추가 (`HTTPConfig{Addr string, Enabled bool}`)
  - `Addr`가 빈 문자열이면 HTTP 서버 미기동 → 기존 E2E 테스트 영향 없음
  - HTTP 전용 E2E 테스트는 임의 포트(`localhost:0`)로 기동하여 포트 충돌 방지

## 에러 응답

- **JSON API**: `{"ok": false, "error": "메시지"}` — IPC Response 포맷과 일관
- **HTMX partial**: 에러 시 인라인 에러 배너 HTML 반환 (토스트 스타일)
- **HTTP 상태 코드**: 400 (잘못된 요청), 404 (없는 리소스), 500 (내부 오류)

## 설정

```toml
[http]
enabled = true               # HTTP 서버 on/off (기본: true)
addr = "localhost:7600"       # 바인딩 주소
```

config 패키지의 기존 TOML 구조에 `[http]` 섹션 추가.
