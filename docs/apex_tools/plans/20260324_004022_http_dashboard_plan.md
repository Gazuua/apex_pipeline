# HTTP 대시보드 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** apex-agent 데몬에 HTTP 서버를 내장하여 실시간 시스템 상태 대시보드를 브라우저에서 제공

**Architecture:** 기존 데몬에 `net/http` 서버를 goroutine으로 병렬 기동. 읽기는 Store 직접 쿼리, 쓰기(2차)는 Router.Dispatch() 경유. Go `html/template` + HTMX로 서버 사이드 렌더링. 외부 의존성 0 (stdlib only + HTMX 14KB embed)

**Tech Stack:** Go stdlib `net/http`, `html/template`, `embed`, HTMX 2.0.4

**Spec:** `docs/apex_tools/plans/20260324_004022_http_dashboard_spec.md`

---

## 파일 구조

### 신규 생성

| 파일 | 책임 |
|------|------|
| `internal/httpd/server.go` | HTTP 서버 구조체, Start/Stop, idle 미들웨어 |
| `internal/httpd/routes.go` | 라우트 등록 (페이지 + partials + JSON API) |
| `internal/httpd/render.go` | 템플릿 렌더링 + JSON 응답 헬퍼 |
| `internal/httpd/handler_dashboard.go` | 메인 대시보드 핸들러 |
| `internal/httpd/handler_backlog.go` | 백로그 페이지/partial 핸들러 |
| `internal/httpd/handler_handoff.go` | 핸드오프 페이지/partial 핸들러 |
| `internal/httpd/handler_queue.go` | 큐 페이지/partial 핸들러 |
| `internal/httpd/queries.go` | 대시보드용 DB 쿼리 함수 (Store 직접) |
| `internal/httpd/server_test.go` | 서버 단위 테스트 |
| `internal/httpd/handler_test.go` | 핸들러 단위 테스트 |
| `internal/httpd/static/htmx.min.js` | HTMX 2.0.4 (BSD 2-Clause) |
| `internal/httpd/static/style.css` | 다크 테마 CSS |
| `internal/httpd/templates/layout.html` | 베이스 레이아웃 |
| `internal/httpd/templates/dashboard.html` | 메인 대시보드 |
| `internal/httpd/templates/backlog.html` | 백로그 페이지 |
| `internal/httpd/templates/handoff.html` | 핸드오프 페이지 |
| `internal/httpd/templates/queue.html` | 큐 페이지 |
| `internal/httpd/templates/partials/summary.html` | 대시보드 요약 카드 |
| `internal/httpd/templates/partials/active_branches.html` | 활성 브랜치 |
| `internal/httpd/templates/partials/queue_status.html` | 큐 상태 |
| `internal/httpd/templates/partials/recent_history.html` | 최근 이력 |
| `internal/httpd/templates/partials/backlog_table.html` | 백로그 테이블 |
| `internal/httpd/templates/partials/handoff_detail.html` | 핸드오프 상세 |
| `e2e/http_test.go` | HTTP E2E 테스트 |

### 수정

| 파일 | 변경 내용 |
|------|-----------|
| `internal/config/config.go` | `HTTPConfig` 구조체 + `Config.HTTP` 필드 + `Defaults()` |
| `internal/daemon/daemon.go` | `Daemon` 구조체에 httpd 필드, `Run()`에 HTTP 기동/idle/shutdown 추가 |
| `internal/cli/daemon_cmd.go` | `daemonRunCmd()`에서 HTTP 설정 전달 |
| `e2e/testenv/env.go` | `NewEnv()`에서 HTTP 서버 기동 설정 |

---

## Task 1: Config에 HTTP 설정 추가

**Files:**
- Modify: `internal/config/config.go`

- [ ] **Step 1: config.go에 HTTPConfig 구조체 및 Config 필드 추가**

`config.go`의 Config 구조체에 `HTTP HTTPConfig` 필드 추가. HTTPConfig 정의:

```go
type HTTPConfig struct {
	Enabled bool   `toml:"enabled"`
	Addr    string `toml:"addr"`
}
```

- [ ] **Step 2: Defaults()에 HTTP 기본값 추가**

```go
HTTP: HTTPConfig{
	Enabled: true,
	Addr:    "localhost:7600",
},
```

- [ ] **Step 3: 테스트 — 기존 config 테스트가 깨지지 않는지 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/config/... -v -count=1`
Expected: PASS (기존 테스트 + 새 기본값 호환)

- [ ] **Step 4: 커밋**

```bash
git add internal/config/config.go
git commit -m "feat(tools): HTTP 서버 config 추가 (enabled, addr)"
```

---

## Task 2: httpd 패키지 — 서버 뼈대 + 테스트

**Files:**
- Create: `internal/httpd/server.go`
- Create: `internal/httpd/server_test.go`

- [ ] **Step 1: 실패하는 테스트 작성**

`server_test.go`:

```go
package httpd

import (
	"net/http"
	"testing"
	"time"
)

func TestServer_StartStop(t *testing.T) {
	srv := New(nil, nil, "localhost:0") // store, router nil OK for this test
	if err := srv.Start(); err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	defer srv.Stop()

	// 서버가 응답하는지 확인
	resp, err := http.Get("http://" + srv.Addr() + "/health")
	if err != nil {
		t.Fatalf("GET /health failed: %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}

func TestServer_LastRequestTime(t *testing.T) {
	srv := New(nil, nil, "localhost:0")
	if err := srv.Start(); err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	defer srv.Stop()

	before := time.Now().Unix()
	http.Get("http://" + srv.Addr() + "/health")
	after := time.Now().Unix()

	last := srv.LastRequestTime()
	if last < before || last > after {
		t.Fatalf("LastRequestTime %d not in [%d, %d]", last, before, after)
	}
}
```

- [ ] **Step 2: 테스트 실행 — 실패 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: FAIL (패키지 없음)

- [ ] **Step 3: server.go 구현**

```go
package httpd

import (
	"context"
	"html/template"
	"net"
	"net/http"
	"sync/atomic"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

type Server struct {
	store       *store.Store
	router      *daemon.Router
	httpSrv     *http.Server
	listener    net.Listener
	lastRequest atomic.Int64
	tmpl        *template.Template // lazy load — Task 4에서 초기화
	addr        string             // 실제 바인딩된 주소 (포트 0 시 할당된 포트)
}

func New(st *store.Store, router *daemon.Router, addr string) *Server {
	s := &Server{}
	mux := http.NewServeMux()

	// health check (템플릿 불필요)
	mux.HandleFunc("GET /health", s.handleHealth)

	s.httpSrv = &http.Server{
		Handler:      s.idleResetMiddleware(mux),
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
	}
	s.store = st
	s.router = router
	s.addr = addr
	return s
}

// InitTemplates — Task 4에서 호출. embed 파일이 준비된 후 lazy 로드.
// 템플릿 미초기화 상태에서 render() 호출 시 에러 배너 반환.
func (s *Server) InitTemplates() error {
	tmpl, err := loadTemplates()
	if err != nil {
		return err
	}
	s.tmpl = tmpl
	return nil
}

func (s *Server) Start() error {
	ln, err := net.Listen("tcp", s.addr)
	if err != nil {
		return err
	}
	s.listener = ln
	s.addr = ln.Addr().String()
	go s.httpSrv.Serve(ln)
	return nil
}

func (s *Server) Stop() error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.httpSrv.Shutdown(ctx)
}

func (s *Server) Addr() string {
	return s.addr
}

func (s *Server) LastRequestTime() int64 {
	return s.lastRequest.Load()
}

func (s *Server) idleResetMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.lastRequest.Store(time.Now().Unix())
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true,"status":"healthy"}`))
}
```

- [ ] **Step 4: 테스트 통과 확인**

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): httpd 패키지 뼈대 — 서버 Start/Stop + health check"
```

---

## Task 3: 데몬에 HTTP 서버 통합

**Files:**
- Modify: `internal/daemon/daemon.go` (struct, Run, shutdown)
- Modify: `internal/cli/daemon_cmd.go` (config 전달)

- [ ] **Step 1: daemon.go — Daemon 구조체에 httpd 필드 추가**

`Daemon` 구조체(line ~28)에 추가:

```go
httpServer *httpd.Server
```

- [ ] **Step 2: daemon.go — Config 구조체에 HTTPConfig 임베드**

`daemon.Config`(line ~21)에 `config.HTTPConfig`를 직접 임베드하여 필드별 복사 방지:

```go
HTTP config.HTTPConfig
```

- [ ] **Step 3: daemon.go — Run()에 HTTP 서버 시작 로직 추가**

IPC 서버 시작 직후(line ~120 이후)에:

```go
if d.cfg.HTTP.Enabled && d.cfg.HTTP.Addr != "" {
	d.httpServer = httpd.New(d.store, d.router, d.cfg.HTTP.Addr)
	if err := d.httpServer.Start(); err != nil {
		ml.Warn("HTTP server failed to start, dashboard unavailable", "error", err)
		d.httpServer = nil
	} else {
		ml.Info("HTTP server started", "addr", d.httpServer.Addr())
	}
}
```

- [ ] **Step 4: daemon.go — idle 타이머에 HTTP lastRequest 통합**

idle 루프(line ~128)에서 `d.server.LastRequestTime()` 이후:

```go
last := d.server.LastRequestTime()
if d.httpServer != nil {
	if httpLast := d.httpServer.LastRequestTime(); httpLast > last {
		last = httpLast
	}
}
```

- [ ] **Step 5: daemon.go — shutdown 순서에 HTTP 추가**

shutdown 레이블(line ~154) 최상단에:

```go
if d.httpServer != nil {
	if err := d.httpServer.Stop(); err != nil {
		ml.Warn("HTTP server stop error", "error", err)
	}
}
```

- [ ] **Step 6: daemon_cmd.go — HTTP 설정 전달**

`daemonRunCmd()`에서 `daemon.Config` 생성 시 (HTTPConfig 임베드이므로 한 줄):

```go
HTTP: appCfg.HTTP,
```

- [ ] **Step 7: 전체 테스트 확인**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: PASS

- [ ] **Step 8: 커밋**

```bash
git add internal/daemon/ internal/cli/
git commit -m "feat(tools): 데몬에 HTTP 서버 통합 — 기동/idle/shutdown"
```

---

## Task 4: 템플릿 렌더링 인프라 + static embed

**Files:**
- Create: `internal/httpd/render.go`
- Create: `internal/httpd/static/style.css`
- Create: `internal/httpd/static/htmx.min.js`
- Create: `internal/httpd/templates/layout.html`

- [ ] **Step 1: render.go — 템플릿 로더 + 렌더링 헬퍼**

```go
package httpd

import (
	"embed"
	"html/template"
	"io"
	"io/fs"
	"net/http"
	"encoding/json"
)

//go:embed templates/*
var templateFS embed.FS

//go:embed static/*
var staticFS embed.FS

func loadTemplates() (*template.Template, error) {
	return template.New("").ParseFS(templateFS, "templates/*.html", "templates/partials/*.html")
}

func (s *Server) render(w http.ResponseWriter, name string, data any) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := s.tmpl.ExecuteTemplate(w, name, data); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

func (s *Server) renderJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}

func (s *Server) renderError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": msg})
}

// renderHTMXError — HTMX partial 에러 시 인라인 에러 배너 HTML 반환
func (s *Server) renderHTMXError(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusInternalServerError)
	fmt.Fprintf(w, `<div class="error-banner">%s</div>`, template.HTMLEscapeString(msg))
}
```

- [ ] **Step 2: New()에서 InitTemplates() 호출 + static 서빙 추가**

`New()` 함수에서 `s.InitTemplates()` 호출 (이제 템플릿 파일이 존재).
실패 시 warn 로그 + 템플릿 없이 계속 (health/static/JSON API는 동작).

또한 `New()`의 mux에 static 파일 서빙과 registerRoutes() 호출 추가.

- [ ] **Step 3: static 파일 서빙 라우트 추가**

`New()`의 mux에:

```go
staticSub, _ := fs.Sub(staticFS, "static")
mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(http.FS(staticSub))))
```

- [ ] **Step 4: layout.html — 쇼케이스 다크 테마 베이스 레이아웃 작성**

다크 테마 (`#0F172A` 배경) + 탑 네비게이션 바 + HTMX 로드 + 폴링 속도 조절 UI.
`{{block "content" .}}{{end}}` 으로 페이지별 콘텐츠 삽입 영역.

- [ ] **Step 5: style.css — 디자인 토큰 기반 CSS 작성**

쇼케이스 페이지(`docs/showcase/agent_ecosystem.html`)에서 디자인 토큰 추출하여 CSS 변수 정의 + 기본 컴포넌트 스타일.

- [ ] **Step 6: htmx.min.js 다운로드 + embed**

HTMX 2.0.4 minified 파일을 `internal/httpd/static/htmx.min.js`에 배치.

- [ ] **Step 7: 테스트 — static 파일 서빙 확인**

```go
func TestServer_StaticFiles(t *testing.T) {
	srv := New(nil, nil, "localhost:0")
	srv.Start()
	defer srv.Stop()

	resp, _ := http.Get("http://" + srv.Addr() + "/static/htmx.min.js")
	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}
```

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 8: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 템플릿 렌더링 인프라 + 다크 테마 CSS + HTMX embed"
```

---

## Task 5: DB 쿼리 함수 — 대시보드 데이터 조회 + 단위 테스트

**Files:**
- Create: `internal/httpd/queries.go`
- Create: `internal/httpd/queries_test.go`

- [ ] **Step 1: queries.go — 대시보드 요약 데이터 쿼리**

```go
package httpd

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"

type DashboardSummary struct {
	BacklogOpen     int
	BacklogFixing   int
	BacklogResolved int
	// severity 분포
	CriticalCount   int
	MajorCount      int
	MinorCount      int
	ActiveBranches  int
	BuildLocked     bool
	MergeLocked     bool
}

func queryDashboardSummary(st *store.Store) (*DashboardSummary, error) {
	// SELECT status, COUNT(*) FROM backlog_items GROUP BY status
	// SELECT COUNT(*) FROM active_branches
	// SELECT ... FROM queue WHERE channel IN ('build','merge') AND locked = 1
}
```

- [ ] **Step 2: queries.go — 백로그 목록 쿼리 (필터 + 정렬)**

```go
type BacklogFilter struct {
	Status    string
	Severity  string
	Timeframe string
	Scope     string
	Type      string
	SortBy    string
	SortDir   string
}

type BacklogItem struct {
	ID          int
	Title       string
	Severity    string
	Timeframe   string
	Scope       string
	Type        string
	Status      string
	Description string
	Related     string
	CreatedAt   string
	UpdatedAt   string
}

func queryBacklogList(st *store.Store, f BacklogFilter) ([]BacklogItem, error) { ... }
```

- [ ] **Step 3: queries.go — 핸드오프 데이터 쿼리**

```go
type ActiveBranch struct {
	ID          int
	WorkspaceID string
	GitBranch   string
	Summary     string
	Status      string
	Scopes      string
	Backlogs    []int  // branch_backlogs JOIN
	StartedAt   string
}

type BranchHistory struct {
	ID          int
	GitBranch   string
	Summary     string
	Action      string // merged | dropped
	Reason      string
	CompletedAt string
}

func queryActiveBranches(st *store.Store) ([]ActiveBranch, error) { ... }
func queryBranchHistory(st *store.Store, limit int) ([]BranchHistory, error) { ... }
```

- [ ] **Step 4: queries.go — 큐 상태 쿼리**

```go
type QueueStatus struct {
	Channel   string
	Locked    bool
	Owner     string
	LockedAt  string
}

func queryQueueStatus(st *store.Store) ([]QueueStatus, error) { ... }
```

- [ ] **Step 5: queries_test.go — 쿼리 단위 테스트**

인메모리 SQLite로 테스트 DB 생성, 마이그레이션 실행, 테스트 데이터 삽입 후 각 쿼리 함수 검증:

```go
func TestQueryDashboardSummary(t *testing.T) {
	st := setupTestStore(t) // 인메모리 SQLite + 마이그레이션
	// INSERT 테스트 데이터: backlog_items 3건 (OPEN 1, FIXING 1, RESOLVED 1)
	summary, err := queryDashboardSummary(st)
	if err != nil { t.Fatal(err) }
	if summary.BacklogOpen != 1 { t.Errorf("expected 1 open, got %d", summary.BacklogOpen) }
}

func TestQueryBacklogList_Filter(t *testing.T) {
	st := setupTestStore(t)
	// INSERT: CRITICAL 2건, MAJOR 1건
	items, err := queryBacklogList(st, BacklogFilter{Severity: "CRITICAL"})
	if err != nil { t.Fatal(err) }
	if len(items) != 2 { t.Errorf("expected 2, got %d", len(items)) }
}
```

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1 -run TestQuery`
Expected: PASS

- [ ] **Step 6: 커밋**

```bash
git add internal/httpd/queries.go internal/httpd/queries_test.go
git commit -m "feat(tools): 대시보드 DB 쿼리 함수 + 단위 테스트"
```

---

## Task 6: 메인 대시보드 페이지 + partials

**Files:**
- Create: `internal/httpd/handler_dashboard.go`
- Create: `internal/httpd/templates/dashboard.html`
- Create: `internal/httpd/templates/partials/summary.html`
- Create: `internal/httpd/templates/partials/active_branches.html`
- Create: `internal/httpd/templates/partials/queue_status.html`
- Create: `internal/httpd/templates/partials/recent_history.html`
- Modify: `internal/httpd/routes.go` (라우트 등록)

- [ ] **Step 1: routes.go 생성 — 라우트 등록 함수**

```go
package httpd

func (s *Server) registerRoutes(mux *http.ServeMux) {
	// Pages
	mux.HandleFunc("GET /", s.handleDashboard)
	// Partials
	mux.HandleFunc("GET /partials/summary", s.handlePartialSummary)
	mux.HandleFunc("GET /partials/active-branches", s.handlePartialActiveBranches)
	mux.HandleFunc("GET /partials/queue-status", s.handlePartialQueueStatus)
	mux.HandleFunc("GET /partials/recent-history", s.handlePartialRecentHistory)
}
```

`New()`에서 `s.registerRoutes(mux)` 호출하도록 리팩터.

- [ ] **Step 2: handler_dashboard.go — 메인 대시보드 핸들러**

```go
func (s *Server) handleDashboard(w http.ResponseWriter, r *http.Request) {
	summary, err := queryDashboardSummary(s.store)
	if err != nil {
		s.renderHTMXError(w, "dashboard query failed: "+err.Error())
		return
	}
	branches, err := queryActiveBranches(s.store)
	if err != nil {
		s.renderHTMXError(w, "branches query failed: "+err.Error())
		return
	}
	queue, _ := queryQueueStatus(s.store)
	history, _ := queryBranchHistory(s.store, 10)
	s.render(w, "dashboard.html", map[string]any{
		"Summary":  summary,
		"Branches": branches,
		"Queue":    queue,
		"History":  history,
	})
}
```

각 partial 핸들러도 동일 패턴 — 해당 쿼리 실행, 에러 시 `renderHTMXError()`, 성공 시 partial 템플릿 렌더.

- [ ] **Step 3: dashboard.html 작성**

layout.html을 extends. 4개 섹션 (요약 카드, 활성 브랜치, 큐, 최근 이력)에 `hx-get` + `hx-trigger="every 1s"` + `data-poll` 적용.

- [ ] **Step 4: partials 4개 작성**

`summary.html` — 백로그 건수 stat 카드 (green/amber/blue 색상)
`active_branches.html` — 상태 머신 뱃지 + 연결 백로그
`queue_status.html` — build/merge 잠금 표시
`recent_history.html` — 타임라인 형태 이력

- [ ] **Step 5: 테스트 — 대시보드 페이지 응답 확인**

```go
func TestHandler_Dashboard(t *testing.T) {
	// testenv로 실제 DB 포함 서버 기동
	// GET / → 200, Content-Type: text/html
	// 응답에 "Dashboard" 포함 확인
}
```

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 6: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 메인 대시보드 페이지 + 4개 partial (1초 폴링)"
```

---

## Task 7: 백로그 페이지

**Files:**
- Create: `internal/httpd/handler_backlog.go`
- Create: `internal/httpd/templates/backlog.html`
- Create: `internal/httpd/templates/partials/backlog_table.html`
- Modify: `internal/httpd/routes.go`

- [ ] **Step 1: routes.go에 백로그 라우트 추가**

```go
mux.HandleFunc("GET /backlog", s.handleBacklog)
mux.HandleFunc("GET /partials/backlog-table", s.handlePartialBacklogTable)
// JSON API
mux.HandleFunc("GET /api/backlog", s.handleAPIBacklog)
```

- [ ] **Step 2: handler_backlog.go 구현**

`handleBacklog` — 전체 페이지, 초기 데이터 포함
`handlePartialBacklogTable` — 쿼리 파라미터로 필터링, 테이블 HTML 조각 반환
`handleAPIBacklog` — JSON 응답

- [ ] **Step 3: backlog.html + partials/backlog_table.html 작성**

필터 UI (HTMX `hx-get` + `hx-include` 로 필터 값 전송), 테이블, 상세 보기 토글.

- [ ] **Step 4: 테스트**

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 백로그 페이지 — 필터/정렬 + JSON API"
```

---

## Task 8: 핸드오프 페이지

**Files:**
- Create: `internal/httpd/handler_handoff.go`
- Create: `internal/httpd/templates/handoff.html`
- Create: `internal/httpd/templates/partials/handoff_detail.html`
- Modify: `internal/httpd/routes.go`

- [ ] **Step 1: routes.go에 핸드오프 라우트 추가**

```go
mux.HandleFunc("GET /handoff", s.handleHandoff)
mux.HandleFunc("GET /partials/handoff-detail/{id}", s.handlePartialHandoffDetail)
mux.HandleFunc("GET /api/handoff", s.handleAPIHandoff)
```

- [ ] **Step 2: handler_handoff.go 구현**

활성 브랜치 목록 + 상태 머신 시각화 (started → design-notified → implementing 뱃지).
`branch_backlogs` JOIN으로 각 브랜치의 연결 백로그 표시.
`branch_history` 탭.

- [ ] **Step 3: handoff.html + partials 작성**

상태 머신 시각화: 쇼케이스의 `.sm-state` 스타일 계승.

- [ ] **Step 4: 테스트**

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 핸드오프 페이지 — 상태 머신 시각화 + 이력"
```

---

## Task 9: 큐 페이지

**Files:**
- Create: `internal/httpd/handler_queue.go`
- Create: `internal/httpd/templates/queue.html`
- Modify: `internal/httpd/routes.go`

- [ ] **Step 1: routes.go에 큐 라우트 추가**

```go
mux.HandleFunc("GET /queue", s.handleQueue)
mux.HandleFunc("GET /api/queue", s.handleAPIQueue)
```

- [ ] **Step 2: handler_queue.go + queue.html**

채널별 잠금 상태 카드. 잠금 중이면 `--accent-red`, 해제면 `--accent-green`.

- [ ] **Step 3: 테스트**

Run: `cd apex_tools/apex-agent && go test ./internal/httpd/... -v -count=1`
Expected: PASS

- [ ] **Step 4: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 큐 페이지 — 잠금 상태 시각화"
```

---

## Task 10: 폴링 속도 조절 UI

**Files:**
- Modify: `internal/httpd/templates/layout.html`
- Modify: `internal/httpd/static/style.css`

- [ ] **Step 1: layout.html 네비바에 폴링 속도 버튼 그룹 추가**

```html
<div class="poll-rate">
  <button onclick="setPollRate('500ms')" class="rate-btn" data-rate="500ms">Fast</button>
  <button onclick="setPollRate('1s')" class="rate-btn active" data-rate="1s">Normal</button>
  <button onclick="setPollRate('2s')" class="rate-btn" data-rate="2s">Slow</button>
</div>
<script>
function setPollRate(rate) {
    localStorage.setItem('pollRate', rate);
    document.querySelectorAll('[data-poll]').forEach(el => {
        el.setAttribute('hx-trigger', 'every ' + rate);
        htmx.process(el);
    });
    document.querySelectorAll('.rate-btn').forEach(b => b.classList.remove('active'));
    document.querySelector('[data-rate="'+rate+'"]').classList.add('active');
}
// 페이지 로드 시 localStorage에서 복원
document.addEventListener('DOMContentLoaded', () => {
    const saved = localStorage.getItem('pollRate');
    if (saved) setPollRate(saved);
});
</script>
```

- [ ] **Step 2: style.css에 폴링 속도 버튼 스타일 추가**

- [ ] **Step 3: 커밋**

```bash
git add internal/httpd/
git commit -m "feat(tools): 폴링 속도 조절 UI (Fast/Normal/Slow + localStorage)"
```

---

## Task 11: E2E 테스트

**Files:**
- Modify: `e2e/testenv/env.go`
- Create: `e2e/http_test.go`

- [ ] **Step 1: testenv/env.go — HTTP 서버 기동 설정 추가**

`NewEnv()`의 `daemon.Config`에:

```go
HTTPEnabled: true,
HTTPAddr:    "localhost:0",  // 임의 포트
```

`Env` 구조체에 `HTTPAddr string` 필드 추가 — 테스트에서 HTTP URL 접근용.

- [ ] **Step 2: http_test.go — E2E 테스트 작성**

```go
func TestHTTP_HealthCheck(t *testing.T) {
	env := testenv.New(t)
	resp, _ := http.Get("http://" + env.HTTPAddr + "/health")
	// 200, {"ok":true,"status":"healthy"}
}

func TestHTTP_DashboardPage(t *testing.T) {
	env := testenv.New(t)
	resp, _ := http.Get("http://" + env.HTTPAddr + "/")
	// 200, Content-Type: text/html, 응답에 "Dashboard" 포함
}

func TestHTTP_BacklogAPI(t *testing.T) {
	env := testenv.New(t)
	// IPC로 backlog add → GET /api/backlog → JSON 응답에 항목 포함
}

func TestHTTP_HandoffAPI(t *testing.T) {
	env := testenv.New(t)
	// GET /api/handoff → 200, JSON 배열
}

func TestHTTP_QueueAPI(t *testing.T) {
	env := testenv.New(t)
	// GET /api/queue → 200, JSON 배열
}

func TestHTTP_PortConflict(t *testing.T) {
	// 포트를 미리 점유 → 데몬 시작 → HTTP 실패해도 IPC는 동작 확인
	ln, _ := net.Listen("tcp", "localhost:0")
	defer ln.Close()
	// env에 해당 포트 설정 → IPC 요청 성공 확인
}

func TestHTTP_ErrorFormat(t *testing.T) {
	env := testenv.New(t)
	resp, _ := http.Get("http://" + env.HTTPAddr + "/api/nonexistent")
	// 404, {"ok":false,"error":"..."}
}
```

- [ ] **Step 3: 전체 테스트 실행**

Run: `cd apex_tools/apex-agent && go test ./... -count=1`
Expected: PASS

- [ ] **Step 4: 커밋**

```bash
git add e2e/ internal/httpd/
git commit -m "test(tools): HTTP 대시보드 E2E 테스트"
```

---

## Task 12: 최종 정리 + 빌드 검증

**Files:**
- 전체 정리

- [ ] **Step 1: clang-format (C++ 소스 변경 없으므로 스킵 확인)**

Go 전용 변경이므로 clang-format 불필요.

- [ ] **Step 2: Go 전체 테스트**

Run: `cd apex_tools/apex-agent && go test ./... -count=1 -race`
Expected: PASS

- [ ] **Step 3: C++ 빌드 (Go 변경만이므로 빌드 스킵 조건 평가)**

Go 전용 변경. C++ 소스 미변경. 빌드 스킵 가능.

- [ ] **Step 4: 최종 커밋 + 푸시**

```bash
git push
```
