# apex-agent Phase 0: 기반 인프라 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** apex-agent 데몬 기반 인프라를 구축하여, 빈 모듈을 등록하고 CLI에서 IPC 왕복 통신이 되는 상태까지 완성한다.

**Architecture:** 싱글톤 데몬이 Named Pipe(Windows)/Unix Socket(Linux)으로 IPC 요청을 수신, 모듈별 라우터로 디스패치, SQLite(WAL)에 상태를 저장한다. CLI는 thin client로 데몬에 JSON 요청만 전달한다.

**Tech Stack:** Go 1.23+, `modernc.org/sqlite` (pure Go SQLite), `github.com/spf13/cobra` (CLI), `github.com/Microsoft/go-winio` (Windows Named Pipe), `log/slog` (내장 구조화 로깅)

**Spec:** `docs/apex_tools/plans/20260322_100656_apex_agent_design_spec.md`

**주의:** 모든 Go 소스 파일 첫 줄에 MIT 라이선스 헤더 필수:
`// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.`

---

## File Map

모든 경로는 `apex_tools/apex-agent/` 기준.

### 생성 파일

| 파일 | 책임 | Task |
|------|------|:----:|
| `go.mod` | 모듈 선언 + 의존성 | 1 |
| `Makefile` | build, test, lint, cross-compile | 1 |
| `.gitignore` | 빌드 산출물 제외 | 1 |
| `cmd/apex-agent/main.go` | 엔트리포인트 | 10 |
| `internal/platform/env.go` | 데이터 디렉토리, 소켓 경로, PID 경로 | 2 |
| `internal/platform/env_test.go` | | 2 |
| `internal/platform/path.go` | 경로 정규화 유틸 | 2 |
| `internal/platform/path_test.go` | | 2 |
| `internal/platform/pid.go` | 프로세스 생존 체크 (크로스플랫폼) | 2 |
| `internal/platform/pid_windows.go` | Windows용 PID 체크 | 2 |
| `internal/platform/pid_unix.go` | Unix용 PID 체크 | 2 |
| `internal/platform/pid_test.go` | | 2 |
| `internal/log/log.go` | slog 래퍼 (설정 가능한 단일 출력) | 3 |
| `internal/log/log_test.go` | | 3 |
| `internal/store/store.go` | SQLite 연결, WAL, 트랜잭션 헬퍼 | 4 |
| `internal/store/store_test.go` | | 4 |
| `internal/store/migrator.go` | 모듈별 버전 기반 마이그레이션 | 4 |
| `internal/store/migrator_test.go` | | 4 |
| `internal/daemon/module.go` | Module interface, RouteRegistrar, HandlerFunc | 5 |
| `internal/daemon/router.go` | 요청→모듈 라우팅 | 5 |
| `internal/daemon/router_test.go` | | 5 |
| `internal/ipc/protocol.go` | Request/Response 타입, 길이 접두 JSON 프레이밍 | 6 |
| `internal/ipc/protocol_test.go` | | 6 |
| `internal/ipc/transport.go` | Transport 인터페이스 | 7 |
| `internal/ipc/transport_windows.go` | Named Pipe 구현 | 7 |
| `internal/ipc/transport_unix.go` | Unix Socket 구현 | 7 |
| `internal/ipc/transport_test.go` | 통합 테스트 | 7 |
| `internal/ipc/server.go` | IPC 리스너, 요청 수신→디스패치→응답 | 8 |
| `internal/ipc/server_test.go` | | 8 |
| `internal/ipc/client.go` | IPC 다이얼러, 요청 송신→응답 수신, 자동 데몬 시작 | 8 |
| `internal/ipc/client_test.go` | | 8 |
| `internal/daemon/daemon.go` | 데몬 수명 관리, PID, 유휴 타임아웃, 모듈 레지스트리 | 9 |
| `internal/daemon/daemon_test.go` | | 9 |
| `internal/cli/root.go` | cobra 루트 커맨드 + 버전 | 10 |
| `internal/cli/daemon_cmd.go` | daemon run/start/stop/status | 10 |
| `internal/cli/detach_unix.go` | Unix 프로세스 분리 (Setsid) | 10 |
| `internal/cli/detach_windows.go` | Windows 프로세스 분리 (CREATE_NEW_PROCESS_GROUP) | 10 |
| `e2e/e2e_test.go` | 전체 경로 스모크 테스트 | 12 |

### 수정 파일

| 파일 | 변경 내용 | Task |
|------|-----------|:----:|
| `.github/workflows/ci.yml` | Go 빌드+테스트 스텝 추가 | 11 |

---

## 의존성 그래프

```
Task 1 (Scaffolding)
  ├→ Task 2 (Platform)
  │    └→ Task 4 (Store) ──→ Task 5 (Module+Router) ──→ Task 9 (Daemon)
  ├→ Task 3 (Log) ──────────────────────────────────────→ Task 9
  ├→ Task 6 (Protocol) ──→ Task 8 (Server+Client) ────→ Task 9
  └→ Task 7 (Transport) ─→ Task 8                 Task 10 (CLI) → Task 12 (E2E)
                                                   Task 11 (CI)
```

Tasks 2, 3, 6, 7은 독립 — 병렬 실행 가능.

---

## Task 1: 프로젝트 스캐폴딩

**Files:**
- Create: `go.mod`, `Makefile`, `.gitignore`, 디렉토리 구조

- [ ] **Step 1: 디렉토리 생성**

```bash
cd apex_tools
mkdir -p apex-agent/cmd/apex-agent
mkdir -p apex-agent/internal/{platform,log,store,daemon,ipc,cli,modules}
```

- [ ] **Step 2: go.mod 초기화**

```bash
cd apex-agent
go mod init github.com/Gazuua/apex_pipeline/apex_tools/apex-agent
```

- [ ] **Step 3: 의존성 추가**

```bash
go get modernc.org/sqlite
go get github.com/spf13/cobra
go get github.com/Microsoft/go-winio
go get golang.org/x/sys
go mod tidy
```

- [ ] **Step 4: Makefile 작성**

```makefile
.PHONY: build test lint clean

BINARY := apex-agent
GOFLAGS := -trimpath -ldflags="-s -w"

build:
	go build $(GOFLAGS) -o $(BINARY)$(if $(filter windows,$(GOOS)),.exe) ./cmd/apex-agent

test:
	go test ./... -race -cover -v

lint:
	go vet ./...

clean:
	rm -f $(BINARY) $(BINARY).exe

# Cross-compile for CI release
release-windows:
	GOOS=windows GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY).exe ./cmd/apex-agent

release-linux:
	GOOS=linux GOARCH=amd64 go build $(GOFLAGS) -o $(BINARY) ./cmd/apex-agent
```

- [ ] **Step 5: .gitignore 작성**

```
apex-agent
apex-agent.exe
```

**참고:** Makefile은 GNU Make를 전제로 한다 (`$(if)` 함수 사용). Windows에서는 MSYS2/MinGW의 GNU Make 사용.

- [ ] **Step 6: 빈 main.go로 빌드 확인**

```go
package main

func main() {}
```

Run: `go build ./cmd/apex-agent`
Expected: 에러 없음

- [ ] **Step 7: 커밋**

```bash
git add apex_tools/apex-agent/
git commit -m "feat(tools): BACKLOG-126 apex-agent Go 프로젝트 스캐폴딩"
```

**리마인더:** 이후 모든 Task에서 Go 소스 파일 생성 시 첫 줄에 MIT 라이선스 헤더를 반드시 포함할 것:
`// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.`

---

## Task 2: Platform 패키지

**Files:**
- Create: `internal/platform/env.go`, `env_test.go`, `path.go`, `path_test.go`, `pid.go`, `pid_windows.go`, `pid_unix.go`, `pid_test.go`

### env — 시스템 경로 결정

- [ ] **Step 1: env 테스트 작성**

```go
// internal/platform/env_test.go
package platform

import (
	"os"
	"runtime"
	"strings"
	"testing"
)

func TestDataDir_NotEmpty(t *testing.T) {
	dir := DataDir()
	if dir == "" {
		t.Fatal("DataDir() returned empty string")
	}
	if !strings.Contains(dir, "apex-agent") {
		t.Errorf("DataDir() = %q, want contains 'apex-agent'", dir)
	}
}

func TestDataDir_Windows_UsesLocalAppData(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only")
	}
	dir := DataDir()
	localAppData := os.Getenv("LOCALAPPDATA")
	if !strings.HasPrefix(dir, localAppData) {
		t.Errorf("DataDir() = %q, want prefix %q", dir, localAppData)
	}
}

func TestDBPath_EndsWithDB(t *testing.T) {
	p := DBPath()
	if !strings.HasSuffix(p, "apex-agent.db") {
		t.Errorf("DBPath() = %q, want suffix 'apex-agent.db'", p)
	}
}

func TestPIDFilePath_EndsWithPID(t *testing.T) {
	p := PIDFilePath()
	if !strings.HasSuffix(p, "apex-agent.pid") {
		t.Errorf("PIDFilePath() = %q, want suffix 'apex-agent.pid'", p)
	}
}

func TestSocketPath_Platform(t *testing.T) {
	p := SocketPath()
	if runtime.GOOS == "windows" {
		if !strings.HasPrefix(p, `\\.\pipe\`) {
			t.Errorf("SocketPath() = %q, want Named Pipe path", p)
		}
	} else {
		if !strings.HasSuffix(p, "apex-agent.sock") {
			t.Errorf("SocketPath() = %q, want suffix 'apex-agent.sock'", p)
		}
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

Run: `go test ./internal/platform/... -v -run TestDataDir`
Expected: FAIL — 함수 미정의

- [ ] **Step 3: env.go 구현**

```go
// internal/platform/env.go
package platform

import (
	"os"
	"path/filepath"
	"runtime"
)

const (
	AppName    = "apex-agent"
	pipeName   = `\\.\pipe\apex-agent`
	socketName = "apex-agent.sock"
	dbName     = "apex-agent.db"
	pidName    = "apex-agent.pid"
)

// DataDir returns the platform-specific data directory for apex-agent.
func DataDir() string {
	if runtime.GOOS == "windows" {
		return filepath.Join(os.Getenv("LOCALAPPDATA"), AppName)
	}
	if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, AppName)
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", AppName)
}

func DBPath() string      { return filepath.Join(DataDir(), dbName) }
func PIDFilePath() string { return filepath.Join(DataDir(), pidName) }

func SocketPath() string {
	if runtime.GOOS == "windows" {
		return pipeName
	}
	return filepath.Join(os.TempDir(), socketName)
}

// EnsureDataDir creates the data directory if it doesn't exist.
func EnsureDataDir() error {
	return os.MkdirAll(DataDir(), 0o755)
}
```

- [ ] **Step 4: 테스트 통과 확인**

Run: `go test ./internal/platform/... -v -run "Test(DataDir|DBPath|PIDFile|Socket)"`
Expected: PASS

### path — 경로 정규화

- [ ] **Step 5: path 테스트 작성**

```go
// internal/platform/path_test.go
package platform

import (
	"path/filepath"
	"runtime"
	"testing"
)

func TestNormalizePath_Absolute(t *testing.T) {
	var input, want string
	if runtime.GOOS == "windows" {
		input = `C:\Users\test\file.txt`
		want = `C:\Users\test\file.txt`
	} else {
		input = "/home/test/file.txt"
		want = "/home/test/file.txt"
	}
	got := NormalizePath(input)
	if got != want {
		t.Errorf("NormalizePath(%q) = %q, want %q", input, got, want)
	}
}

func TestNormalizePath_ForwardSlashes(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only — forward slash normalization")
	}
	got := NormalizePath("C:/Users/test/file.txt")
	want := filepath.FromSlash("C:/Users/test/file.txt")
	if got != want {
		t.Errorf("NormalizePath() = %q, want %q", got, want)
	}
}

func TestNormalizePath_MSYSPrefix(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("Windows only — MSYS path handling")
	}
	// MSYS converts /c/Users/... to C:\Users\...
	got := NormalizePath("/c/Users/test")
	if got[1] != ':' {
		t.Errorf("NormalizePath(/c/Users/test) = %q, want drive letter path", got)
	}
}
```

- [ ] **Step 6: path.go 구현**

```go
// internal/platform/path.go
package platform

import (
	"path/filepath"
	"runtime"
	"strings"
)

// NormalizePath normalizes a file path for the current platform.
// On Windows, handles MSYS-style paths (/c/Users/...) and forward slashes.
func NormalizePath(p string) string {
	if runtime.GOOS == "windows" && len(p) >= 3 && p[0] == '/' && p[2] == '/' {
		// MSYS path: /c/Users/... → C:\Users\...
		drive := strings.ToUpper(string(p[1]))
		p = drive + ":" + p[2:]
	}
	return filepath.Clean(filepath.FromSlash(p))
}
```

- [ ] **Step 7: path 테스트 통과 확인**

Run: `go test ./internal/platform/... -v -run TestNormalizePath`
Expected: PASS

### pid — 프로세스 생존 체크

- [ ] **Step 8: pid 테스트 작성**

```go
// internal/platform/pid_test.go
package platform

import (
	"os"
	"testing"
)

func TestIsProcessAlive_Self(t *testing.T) {
	// Current process must be alive.
	if !IsProcessAlive(os.Getpid()) {
		t.Error("IsProcessAlive(self) = false, want true")
	}
}

func TestIsProcessAlive_NonExistent(t *testing.T) {
	// PID -1 is universally invalid across all platforms.
	if IsProcessAlive(-1) {
		t.Error("IsProcessAlive(-1) = true, want false")
	}
}
```

- [ ] **Step 9: pid 구현 (플랫폼별)**

```go
// internal/platform/pid.go
package platform

// IsProcessAlive checks if a process with the given PID exists.
// Platform-specific implementation in pid_windows.go and pid_unix.go.
```

```go
// internal/platform/pid_windows.go
//go:build windows

package platform

import "golang.org/x/sys/windows"

func IsProcessAlive(pid int) bool {
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
	if err != nil {
		return false
	}
	_ = windows.CloseHandle(h)
	return true
}
```

```go
// internal/platform/pid_unix.go
//go:build !windows

package platform

import "syscall"

func IsProcessAlive(pid int) bool {
	err := syscall.Kill(pid, 0)
	return err == nil
}
```

**참고:** `golang.org/x/sys/windows` 의존성 추가 필요: `go get golang.org/x/sys`

- [ ] **Step 10: pid 테스트 통과 확인**

Run: `go test ./internal/platform/... -v -run TestIsProcessAlive`
Expected: PASS

- [ ] **Step 11: 전체 platform 테스트 + 커밋**

Run: `go test ./internal/platform/... -v -race`
Expected: 전부 PASS

```bash
git add internal/platform/
git commit -m "feat(tools): BACKLOG-126 platform 패키지 — env, path, pid"
```

---

## Task 3: Log 패키지

**Files:**
- Create: `internal/log/log.go`, `log_test.go`

- [ ] **Step 1: log 테스트 작성**

```go
// internal/log/log_test.go
package log

import (
	"bytes"
	"strings"
	"testing"
)

func TestNew_WritesToBuffer(t *testing.T) {
	var buf bytes.Buffer
	logger := New(WithWriter(&buf))
	logger.Info("test message", "key", "value")

	out := buf.String()
	if !strings.Contains(out, "test message") {
		t.Errorf("log output = %q, want contains 'test message'", out)
	}
	if !strings.Contains(out, "key=value") && !strings.Contains(out, `"key":"value"`) {
		t.Errorf("log output = %q, want contains key/value", out)
	}
}

func TestNew_DefaultDoesNotPanic(t *testing.T) {
	logger := New()
	logger.Info("should not panic")
}
```

- [ ] **Step 2: 테스트 실패 확인 → 구현**

```go
// internal/log/log.go
package log

import (
	"io"
	"log/slog"
	"os"
)

type Logger = *slog.Logger

type options struct {
	writer io.Writer
	level  slog.Level
}

type Option func(*options)

func WithWriter(w io.Writer) Option { return func(o *options) { o.writer = w } }
func WithLevel(l slog.Level) Option { return func(o *options) { o.level = l } }

func New(opts ...Option) Logger {
	o := &options{
		writer: os.Stderr,
		level:  slog.LevelInfo,
	}
	for _, opt := range opts {
		opt(o)
	}
	return slog.New(slog.NewTextHandler(o.writer, &slog.HandlerOptions{
		Level: o.level,
	}))
}
```

- [ ] **Step 3: 테스트 통과 확인 + 커밋**

Run: `go test ./internal/log/... -v -race`
Expected: PASS

```bash
git add internal/log/
git commit -m "feat(tools): BACKLOG-126 log 패키지 — slog 래퍼"
```

---

## Task 4: Store 패키지 (SQLite + Migrator)

**Files:**
- Create: `internal/store/store.go`, `store_test.go`, `migrator.go`, `migrator_test.go`

**Ref:** 스펙 §5 데이터 모델

### Store — SQLite 연결

- [ ] **Step 1: store 테스트 작성**

```go
// internal/store/store_test.go
package store

import (
	"context"
	"testing"
)

func TestOpen_InMemory(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatalf("Open(:memory:) error: %v", err)
	}
	defer s.Close()
}

func TestExec_CreateTable(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	_, err = s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	if err != nil {
		t.Fatalf("Exec CREATE TABLE error: %v", err)
	}

	_, err = s.Exec("INSERT INTO test (name) VALUES (?)", "hello")
	if err != nil {
		t.Fatalf("Exec INSERT error: %v", err)
	}
}

func TestQuery_SelectRows(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)")
	s.Exec("INSERT INTO test (name) VALUES (?)", "alice")
	s.Exec("INSERT INTO test (name) VALUES (?)", "bob")

	rows, err := s.Query("SELECT name FROM test ORDER BY id")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()

	var names []string
	for rows.Next() {
		var name string
		rows.Scan(&name)
		names = append(names, name)
	}

	if len(names) != 2 || names[0] != "alice" || names[1] != "bob" {
		t.Errorf("got %v, want [alice bob]", names)
	}
}

func TestBeginTx_CommitAndRollback(t *testing.T) {
	s, err := Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	s.Exec("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT)")

	// Commit
	tx, _ := s.BeginTx(context.Background())
	tx.Exec("INSERT INTO test (val) VALUES (?)", "committed")
	tx.Commit()

	// Rollback
	tx2, _ := s.BeginTx(context.Background())
	tx2.Exec("INSERT INTO test (val) VALUES (?)", "rolled_back")
	tx2.Rollback()

	rows, _ := s.Query("SELECT val FROM test")
	defer rows.Close()
	var count int
	for rows.Next() {
		count++
	}
	if count != 1 {
		t.Errorf("got %d rows, want 1 (only committed)", count)
	}
}
```

- [ ] **Step 2: 테스트 실패 확인 → store.go 구현**

```go
// internal/store/store.go
package store

import (
	"context"
	"database/sql"

	_ "modernc.org/sqlite"
)

type Store struct {
	db *sql.DB
}

// Open opens a SQLite database and configures WAL mode.
func Open(dsn string) (*Store, error) {
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	// Enable WAL mode for concurrent reads.
	if _, err := db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		db.Close()
		return nil, err
	}
	// Busy timeout 5 seconds.
	if _, err := db.Exec("PRAGMA busy_timeout=5000"); err != nil {
		db.Close()
		return nil, err
	}
	return &Store{db: db}, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) Exec(query string, args ...any) (sql.Result, error) {
	return s.db.Exec(query, args...)
}

func (s *Store) Query(query string, args ...any) (*sql.Rows, error) {
	return s.db.Query(query, args...)
}

func (s *Store) QueryRow(query string, args ...any) *sql.Row {
	return s.db.QueryRow(query, args...)
}

func (s *Store) BeginTx(ctx context.Context) (*sql.Tx, error) {
	return s.db.BeginTx(ctx, nil)
}
```

- [ ] **Step 3: store 테스트 통과 확인**

Run: `go test ./internal/store/... -v -run "Test(Open|Exec|Query|BeginTx)"`
Expected: PASS

### Migrator — 모듈별 마이그레이션

- [ ] **Step 4: migrator 테스트 작성**

```go
// internal/store/migrator_test.go
package store

import "testing"

func TestMigrate_NewDB(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("test_module", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatalf("Migrate() error: %v", err)
	}

	// Verify table was created.
	_, err := s.Exec("INSERT INTO test_items (id) VALUES (1)")
	if err != nil {
		t.Fatalf("table not created: %v", err)
	}
}

func TestMigrate_Idempotent(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	called := 0
	m := NewMigrator(s)
	m.Register("test_module", 1, func(s *Store) error {
		called++
		_, err := s.Exec("CREATE TABLE test_items (id INTEGER PRIMARY KEY)")
		return err
	})

	m.Migrate()
	m.Migrate() // second call should be a no-op

	if called != 1 {
		t.Errorf("migration called %d times, want 1", called)
	}
}

func TestMigrate_MultiVersion(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("mod", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE items (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("mod", 2, func(s *Store) error {
		_, err := s.Exec("ALTER TABLE items ADD COLUMN name TEXT")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	// Verify v2 migration ran — name column exists.
	_, err := s.Exec("INSERT INTO items (id, name) VALUES (1, 'test')")
	if err != nil {
		t.Fatalf("v2 migration failed: %v", err)
	}
}

func TestMigrate_MultipleModules(t *testing.T) {
	s, _ := Open(":memory:")
	defer s.Close()

	m := NewMigrator(s)
	m.Register("alpha", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE alpha_t (id INTEGER PRIMARY KEY)")
		return err
	})
	m.Register("beta", 1, func(s *Store) error {
		_, err := s.Exec("CREATE TABLE beta_t (id INTEGER PRIMARY KEY)")
		return err
	})

	if err := m.Migrate(); err != nil {
		t.Fatal(err)
	}

	// Both tables exist.
	if _, err := s.Exec("INSERT INTO alpha_t (id) VALUES (1)"); err != nil {
		t.Fatal("alpha table missing")
	}
	if _, err := s.Exec("INSERT INTO beta_t (id) VALUES (1)"); err != nil {
		t.Fatal("beta table missing")
	}
}
```

- [ ] **Step 5: 테스트 실패 확인 → migrator.go 구현**

```go
// internal/store/migrator.go
package store

import (
	"fmt"
	"sort"
)

type MigrateFunc func(s *Store) error

type migration struct {
	module  string
	version int
	fn      MigrateFunc
}

type Migrator struct {
	store      *Store
	migrations []migration
}

func NewMigrator(s *Store) *Migrator {
	return &Migrator{store: s}
}

func (m *Migrator) Register(module string, version int, fn MigrateFunc) {
	m.migrations = append(m.migrations, migration{module, version, fn})
}

func (m *Migrator) Migrate() error {
	// Create version tracking table.
	_, err := m.store.Exec(`CREATE TABLE IF NOT EXISTS _migrations (
		module  TEXT NOT NULL,
		version INTEGER NOT NULL,
		PRIMARY KEY (module, version)
	)`)
	if err != nil {
		return fmt.Errorf("create _migrations: %w", err)
	}

	// Sort by module, then version.
	sort.Slice(m.migrations, func(i, j int) bool {
		if m.migrations[i].module != m.migrations[j].module {
			return m.migrations[i].module < m.migrations[j].module
		}
		return m.migrations[i].version < m.migrations[j].version
	})

	for _, mig := range m.migrations {
		// Check if already applied.
		var count int
		row := m.store.QueryRow(
			"SELECT COUNT(*) FROM _migrations WHERE module=? AND version=?",
			mig.module, mig.version,
		)
		if err := row.Scan(&count); err != nil {
			return err
		}
		if count > 0 {
			continue
		}

		// Apply migration.
		if err := mig.fn(m.store); err != nil {
			return fmt.Errorf("migrate %s v%d: %w", mig.module, mig.version, err)
		}

		// Record version.
		if _, err := m.store.Exec(
			"INSERT INTO _migrations (module, version) VALUES (?, ?)",
			mig.module, mig.version,
		); err != nil {
			return err
		}
	}
	return nil
}
```

- [ ] **Step 6: 전체 store 테스트 통과 + 커밋**

Run: `go test ./internal/store/... -v -race`
Expected: PASS

```bash
git add internal/store/
git commit -m "feat(tools): BACKLOG-126 store 패키지 — SQLite WAL + Migrator"
```

---

## Task 5: Module Interface + Router

**Files:**
- Create: `internal/daemon/module.go`, `router.go`, `router_test.go`

**Ref:** 스펙 §4 계층 아키텍처, Module ↔ ServiceBase 대응표

- [ ] **Step 1: Module interface + 타입 정의**

```go
// internal/daemon/module.go
package daemon

import (
	"context"
	"encoding/json"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// HandlerFunc processes a module action.
// params is the raw JSON from the request; workspace identifies the caller.
// Returns a result value (will be JSON-marshaled) or an error.
type HandlerFunc func(ctx context.Context, params json.RawMessage, workspace string) (any, error)

// RouteRegistrar allows a module to register its action handlers.
type RouteRegistrar interface {
	Handle(action string, handler HandlerFunc)
}

// Module is the interface that all apex-agent modules implement.
// Mirrors the C++ core's ServiceBase lifecycle.
type Module interface {
	// Name returns the module's unique identifier (e.g., "handoff", "queue").
	Name() string

	// RegisterRoutes registers the module's action handlers.
	RegisterRoutes(reg RouteRegistrar)

	// RegisterSchema registers the module's DB migrations.
	RegisterSchema(m *store.Migrator)

	// OnStart is called after all modules are registered and DB is migrated.
	OnStart(ctx context.Context) error

	// OnStop is called during graceful shutdown, in reverse registration order.
	OnStop() error
}
```

이 파일은 순수 인터페이스 정의 — 테스트 불필요.

- [ ] **Step 2: router 테스트 작성**

**설계 주의:** `Response` 타입은 `ipc` 패키지에만 정의한다. Router는 `(any, error)`를 반환하여 IPC 레이어와 순환 의존을 방지한다. `ipc/server.go`가 Router 결과를 `ipc.Response`로 래핑.

```go
// internal/daemon/router_test.go
package daemon

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
)

func TestRouter_Dispatch(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("echo", func(reg RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"pong": ws}, nil
		})
	})

	result, err := r.Dispatch(context.Background(), "echo", "ping", nil, "branch_02")
	if err != nil {
		t.Fatalf("Dispatch error: %v", err)
	}

	data, _ := json.Marshal(result)
	var m map[string]string
	json.Unmarshal(data, &m)
	if m["pong"] != "branch_02" {
		t.Errorf("got %v, want pong=branch_02", m)
	}
}

func TestRouter_Dispatch_UnknownModule(t *testing.T) {
	r := NewRouter()
	_, err := r.Dispatch(context.Background(), "missing", "action", nil, "ws")
	if err == nil {
		t.Error("expected error for unknown module")
	}
}

func TestRouter_Dispatch_UnknownAction(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("echo", func(reg RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return nil, nil
		})
	})

	_, err := r.Dispatch(context.Background(), "echo", "missing", nil, "ws")
	if err == nil {
		t.Error("expected error for unknown action")
	}
}

func TestRouter_Dispatch_HandlerError(t *testing.T) {
	r := NewRouter()
	r.RegisterModule("fail", func(reg RouteRegistrar) {
		reg.Handle("boom", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return nil, errors.New("handler failed")
		})
	})

	_, err := r.Dispatch(context.Background(), "fail", "boom", nil, "ws")
	if err == nil || err.Error() != "handler failed" {
		t.Errorf("expected 'handler failed', got %v", err)
	}
}
```

- [ ] **Step 3: 테스트 실패 확인 → router.go 구현**

```go
// internal/daemon/router.go
package daemon

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
)

// Router는 (any, error)를 반환. Response 타입은 ipc 패키지에만 존재.
// 이렇게 해서 daemon ↔ ipc 순환 의존을 방지한다.

type moduleRoutes struct {
	handlers map[string]HandlerFunc
}

type Router struct {
	mu      sync.RWMutex
	modules map[string]*moduleRoutes
}

func NewRouter() *Router {
	return &Router{modules: make(map[string]*moduleRoutes)}
}

func (r *Router) RegisterModule(name string, setup func(RouteRegistrar)) {
	mr := &moduleRoutes{handlers: make(map[string]HandlerFunc)}
	setup(mr)
	r.mu.Lock()
	r.modules[name] = mr
	r.mu.Unlock()
}

// Dispatch routes a request to the appropriate module handler.
// Returns (result, nil) on success, (nil, error) on failure.
// The IPC server wraps this into an ipc.Response.
func (r *Router) Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error) {
	r.mu.RLock()
	mr, ok := r.modules[module]
	r.mu.RUnlock()

	if !ok {
		return nil, fmt.Errorf("unknown module: %s", module)
	}

	handler, ok := mr.handlers[action]
	if !ok {
		return nil, fmt.Errorf("unknown action: %s.%s", module, action)
	}

	return handler(ctx, params, workspace)
}

func (mr *moduleRoutes) Handle(action string, handler HandlerFunc) {
	mr.handlers[action] = handler
}
```

- [ ] **Step 4: 테스트 통과 확인 + 커밋**

Run: `go test ./internal/daemon/... -v -race`
Expected: PASS

```bash
git add internal/daemon/
git commit -m "feat(tools): BACKLOG-126 Module interface + Router"
```

---

## Task 6: IPC Protocol

**Files:**
- Create: `internal/ipc/protocol.go`, `protocol_test.go`

**Ref:** 스펙 §4 요청 흐름, §6 JSON 프로토콜

프레이밍: 4바이트 빅엔디언 길이 + JSON 바이트. 스트림 위에서 메시지 경계를 명확히 구분.

- [ ] **Step 1: protocol 테스트 작성**

```go
// internal/ipc/protocol_test.go
package ipc

import (
	"bytes"
	"encoding/json"
	"testing"
)

func TestWriteRead_Roundtrip(t *testing.T) {
	var buf bytes.Buffer

	req := &Request{
		Module:    "handoff",
		Action:    "notify",
		Params:    json.RawMessage(`{"type":"start"}`),
		Workspace: "branch_02",
	}

	if err := WriteMessage(&buf, req); err != nil {
		t.Fatal(err)
	}

	var got Request
	if err := ReadMessage(&buf, &got); err != nil {
		t.Fatal(err)
	}

	if got.Module != "handoff" || got.Action != "notify" || got.Workspace != "branch_02" {
		t.Errorf("roundtrip mismatch: %+v", got)
	}
}

func TestWriteRead_Response(t *testing.T) {
	var buf bytes.Buffer

	resp := &Response{
		OK:   true,
		Data: json.RawMessage(`{"id":30}`),
	}

	WriteMessage(&buf, resp)

	var got Response
	ReadMessage(&buf, &got)

	if !got.OK {
		t.Error("OK = false, want true")
	}
}

func TestReadMessage_Empty(t *testing.T) {
	var buf bytes.Buffer
	var req Request
	err := ReadMessage(&buf, &req)
	if err == nil {
		t.Error("expected error on empty buffer")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인 → protocol.go 구현**

```go
// internal/ipc/protocol.go
package ipc

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
)

// Request is the JSON message sent from CLI to daemon.
type Request struct {
	Module    string          `json:"module"`
	Action    string          `json:"action"`
	Params    json.RawMessage `json:"params,omitempty"`
	Workspace string          `json:"workspace"`
}

// Response is the JSON message sent from daemon to CLI.
type Response struct {
	OK    bool            `json:"ok"`
	Data  json.RawMessage `json:"data,omitempty"`
	Error string          `json:"error,omitempty"`
}

const maxMessageSize = 4 * 1024 * 1024 // 4MB safety limit

// WriteMessage serializes v to JSON and writes it with a 4-byte length prefix.
func WriteMessage(w io.Writer, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}
	length := uint32(len(data))
	if err := binary.Write(w, binary.BigEndian, length); err != nil {
		return fmt.Errorf("write length: %w", err)
	}
	if _, err := w.Write(data); err != nil {
		return fmt.Errorf("write payload: %w", err)
	}
	return nil
}

// ReadMessage reads a length-prefixed JSON message and unmarshals into v.
func ReadMessage(r io.Reader, v any) error {
	var length uint32
	if err := binary.Read(r, binary.BigEndian, &length); err != nil {
		return fmt.Errorf("read length: %w", err)
	}
	if length > maxMessageSize {
		return fmt.Errorf("message too large: %d bytes", length)
	}
	data := make([]byte, length)
	if _, err := io.ReadFull(r, data); err != nil {
		return fmt.Errorf("read payload: %w", err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		return fmt.Errorf("unmarshal: %w", err)
	}
	return nil
}
```

- [ ] **Step 3: 테스트 통과 확인 + 커밋**

Run: `go test ./internal/ipc/... -v -race -run "Test(WriteRead|ReadMessage)"`
Expected: PASS

```bash
git add internal/ipc/protocol.go internal/ipc/protocol_test.go
git commit -m "feat(tools): BACKLOG-126 IPC protocol — 길이 접두 JSON 프레이밍"
```

---

## Task 7: IPC Transport

**Files:**
- Create: `internal/ipc/transport.go`, `transport_windows.go`, `transport_unix.go`, `transport_test.go`

- [ ] **Step 1: transport 인터페이스 + 플랫폼 구현**

```go
// internal/ipc/transport.go
package ipc

import "net"

// Listener creates a platform-specific IPC listener.
// Windows: Named Pipe, Unix: Unix Domain Socket.
// Implemented in transport_windows.go and transport_unix.go.
func Listen(addr string) (net.Listener, error) {
	return listenPlatform(addr)
}

// Dial connects to the platform-specific IPC endpoint.
func Dial(addr string) (net.Conn, error) {
	return dialPlatform(addr)
}
```

```go
// internal/ipc/transport_windows.go
//go:build windows

package ipc

import (
	"net"
	"time"

	"github.com/Microsoft/go-winio"
)

func listenPlatform(addr string) (net.Listener, error) {
	return winio.ListenPipe(addr, &winio.PipeConfig{
		SecurityDescriptor: "",        // default ACL
		MessageMode:        false,     // byte stream
		InputBufferSize:    64 * 1024,
		OutputBufferSize:   64 * 1024,
	})
}

func dialPlatform(addr string) (net.Conn, error) {
	return winio.DialPipe(addr, (*time.Duration)(nil))
}
```

```go
// internal/ipc/transport_unix.go
//go:build !windows

package ipc

import (
	"net"
	"os"
)

func listenPlatform(addr string) (net.Listener, error) {
	// Remove stale socket file if exists.
	os.Remove(addr)
	return net.Listen("unix", addr)
}

func dialPlatform(addr string) (net.Conn, error) {
	return net.Dial("unix", addr)
}
```

- [ ] **Step 2: transport 통합 테스트 작성**

```go
// internal/ipc/transport_test.go
package ipc

import (
	"runtime"
	"testing"
)

func testAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-test`
	}
	return "/tmp/apex-agent-test.sock"
}

func TestTransport_ListenAndDial(t *testing.T) {
	addr := testAddr()

	ln, err := Listen(addr)
	if err != nil {
		t.Fatalf("Listen: %v", err)
	}
	defer ln.Close()

	// Accept in goroutine.
	done := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			done <- err
			return
		}
		conn.Write([]byte("hello"))
		conn.Close()
		done <- nil
	}()

	// Dial.
	conn, err := Dial(addr)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}

	buf := make([]byte, 5)
	n, _ := conn.Read(buf)
	conn.Close()

	if string(buf[:n]) != "hello" {
		t.Errorf("got %q, want 'hello'", buf[:n])
	}

	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
```

- [ ] **Step 3: 테스트 통과 확인 + 커밋**

Run: `go test ./internal/ipc/... -v -race -run TestTransport`
Expected: PASS

```bash
git add internal/ipc/transport*.go
git commit -m "feat(tools): BACKLOG-126 IPC transport — Named Pipe + Unix Socket"
```

---

## Task 8: IPC Server + Client

**Files:**
- Create: `internal/ipc/server.go`, `server_test.go`, `client.go`, `client_test.go`

### Server

- [ ] **Step 1: server 테스트 작성**

```go
// internal/ipc/server_test.go
package ipc

import (
	"context"
	"encoding/json"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
)

func TestServer_HandleRequest(t *testing.T) {
	addr := testAddr() + "-server"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-server.sock"
	}

	router := daemon.NewRouter()
	router.RegisterModule("echo", func(reg daemon.RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"reply": "pong"}, nil
		})
	})

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go srv.Serve(ctx)
	time.Sleep(50 * time.Millisecond) // wait for listener

	// Send request via raw transport.
	conn, err := Dial(addr)
	if err != nil {
		t.Fatalf("Dial: %v", err)
	}
	defer conn.Close()

	req := &Request{Module: "echo", Action: "ping", Workspace: "test"}
	WriteMessage(conn, req)

	var resp Response
	ReadMessage(conn, &resp)

	if !resp.OK {
		t.Fatalf("response not OK: %s", resp.Error)
	}

	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		t.Fatal(err)
	}
	if data["reply"] != "pong" {
		t.Errorf("got %v, want reply=pong", data)
	}
}
```

- [ ] **Step 2: 테스트 실패 확인 → server.go 구현**

```go
// internal/ipc/server.go
package ipc

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
)

type Server struct {
	addr        string
	router      *daemon.Router
	listener    net.Listener
	lastRequest atomic.Int64 // unix timestamp — for idle timeout
	wg          sync.WaitGroup
}

func NewServer(addr string, router *daemon.Router) *Server {
	return &Server{addr: addr, router: router}
}

func (s *Server) LastRequestTime() int64 {
	return s.lastRequest.Load()
}

func (s *Server) Serve(ctx context.Context) error {
	ln, err := Listen(s.addr)
	if err != nil {
		return err
	}
	s.listener = ln

	go func() {
		<-ctx.Done()
		ln.Close()
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				s.wg.Wait()
				return nil
			default:
				continue
			}
		}
		s.wg.Add(1)
		go s.handleConn(ctx, conn)
	}
}

func (s *Server) handleConn(ctx context.Context, conn net.Conn) {
	defer s.wg.Done()
	defer conn.Close()

	var req Request
	if err := ReadMessage(conn, &req); err != nil {
		resp := Response{OK: false, Error: err.Error()}
		WriteMessage(conn, &resp)
		return
	}

	s.lastRequest.Store(time.Now().Unix())

	// Router returns (any, error) → wrap into ipc.Response
	result, err := s.router.Dispatch(ctx, req.Module, req.Action, req.Params, req.Workspace)
	if err != nil {
		WriteMessage(conn, &Response{OK: false, Error: err.Error()})
		return
	}

	data, err := json.Marshal(result)
	if err != nil {
		WriteMessage(conn, &Response{OK: false, Error: fmt.Sprintf("marshal: %v", err)})
		return
	}

	WriteMessage(conn, &Response{OK: true, Data: data})
}
```

- [ ] **Step 3: server 테스트 통과 확인**

Run: `go test ./internal/ipc/... -v -race -run TestServer`
Expected: PASS

### Client

- [ ] **Step 4: client 테스트 작성**

```go
// internal/ipc/client_test.go
package ipc

import (
	"context"
	"encoding/json"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
)

func TestClient_Send(t *testing.T) {
	addr := testAddr() + "-client"
	if runtime.GOOS != "windows" {
		addr = "/tmp/apex-agent-test-client.sock"
	}

	router := daemon.NewRouter()
	router.RegisterModule("echo", func(reg daemon.RouteRegistrar) {
		reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
			return map[string]string{"ws": ws}, nil
		})
	})

	srv := NewServer(addr, router)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go srv.Serve(ctx)
	time.Sleep(50 * time.Millisecond)

	client := NewClient(addr)
	resp, err := client.Send(context.Background(), "echo", "ping", nil, "branch_01")
	if err != nil {
		t.Fatalf("Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("not OK: %s", resp.Error)
	}

	var data map[string]string
	json.Unmarshal(resp.Data, &data)
	if data["ws"] != "branch_01" {
		t.Errorf("got %v, want ws=branch_01", data)
	}
}
```

- [ ] **Step 5: 테스트 실패 확인 → client.go 구현**

```go
// internal/ipc/client.go
package ipc

import (
	"context"
	"encoding/json"
	"fmt"
)

type Client struct {
	addr string
}

func NewClient(addr string) *Client {
	return &Client{addr: addr}
}

// Send sends a request to the daemon and returns the response.
func (c *Client) Send(ctx context.Context, module, action string, params any, workspace string) (*Response, error) {
	var rawParams json.RawMessage
	if params != nil {
		var err error
		rawParams, err = json.Marshal(params)
		if err != nil {
			return nil, fmt.Errorf("marshal params: %w", err)
		}
	}

	conn, err := Dial(c.addr)
	if err != nil {
		return nil, fmt.Errorf("dial: %w", err)
	}
	defer conn.Close()

	req := &Request{
		Module:    module,
		Action:    action,
		Params:    rawParams,
		Workspace: workspace,
	}

	if err := WriteMessage(conn, req); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}

	var resp Response
	if err := ReadMessage(conn, &resp); err != nil {
		return nil, fmt.Errorf("read: %w", err)
	}

	return &resp, nil
}
```

- [ ] **Step 6: 전체 IPC 테스트 통과 + 커밋**

Run: `go test ./internal/ipc/... -v -race`
Expected: PASS

```bash
git add internal/ipc/server.go internal/ipc/server_test.go internal/ipc/client.go internal/ipc/client_test.go
git commit -m "feat(tools): BACKLOG-126 IPC Server + Client"
```

---

## Task 9: Daemon Core

**Files:**
- Create: `internal/daemon/daemon.go`, `daemon_test.go`

**Ref:** 스펙 §7 데몬 수명 관리

- [ ] **Step 1: daemon 테스트 작성**

```go
// internal/daemon/daemon_test.go
package daemon

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testSocketAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-daemon-test`
	}
	return "/tmp/apex-agent-daemon-test.sock"
}

func TestDaemon_StartStop(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr(),
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	time.Sleep(100 * time.Millisecond)

	// PID file should exist.
	if _, err := os.Stat(cfg.PIDFilePath); os.IsNotExist(err) {
		t.Error("PID file not created")
	}

	cancel()
	if err := <-done; err != nil {
		t.Errorf("Run returned error: %v", err)
	}

	// PID file should be cleaned up.
	if _, err := os.Stat(cfg.PIDFilePath); !os.IsNotExist(err) {
		t.Error("PID file not cleaned up")
	}
}

func TestDaemon_RegisterModule(t *testing.T) {
	tmpDir := t.TempDir()
	cfg := Config{
		DBPath:      filepath.Join(tmpDir, "test.db"),
		PIDFilePath: filepath.Join(tmpDir, "test.pid"),
		SocketAddr:  testSocketAddr() + "-mod",
		IdleTimeout: 5 * time.Minute,
	}

	d, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}

	started := false
	stopped := false

	d.Register(&mockModule{
		name:    "test",
		onStart: func() { started = true },
		onStop:  func() { stopped = true },
	})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()
	time.Sleep(100 * time.Millisecond)

	if !started {
		t.Error("module OnStart not called")
	}

	cancel()
	<-done

	if !stopped {
		t.Error("module OnStop not called")
	}
}

// mockModule for testing.
type mockModule struct {
	name    string
	onStart func()
	onStop  func()
}

func (m *mockModule) Name() string                       { return m.name }
func (m *mockModule) RegisterRoutes(reg RouteRegistrar)   {}
func (m *mockModule) RegisterSchema(mig *store.Migrator)  {}
func (m *mockModule) OnStart(ctx context.Context) error {
	if m.onStart != nil {
		m.onStart()
	}
	return nil
}
func (m *mockModule) OnStop() error {
	if m.onStop != nil {
		m.onStop()
	}
	return nil
}
```

- [ ] **Step 2: 테스트 실패 확인 → daemon.go 구현**

```go
// internal/daemon/daemon.go
package daemon

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

type Config struct {
	DBPath      string
	PIDFilePath string
	SocketAddr  string
	IdleTimeout time.Duration
}

type Daemon struct {
	cfg     Config
	store   *store.Store
	router  *Router
	server  *ipc.Server
	modules []Module
	logger  log.Logger
}

func New(cfg Config) (*Daemon, error) {
	s, err := store.Open(cfg.DBPath)
	if err != nil {
		return nil, fmt.Errorf("open store: %w", err)
	}

	router := NewRouter()
	logger := log.New()

	return &Daemon{
		cfg:    cfg,
		store:  s,
		router: router,
		logger: logger,
	}, nil
}

func (d *Daemon) Register(m Module) {
	d.modules = append(d.modules, m)
}

func (d *Daemon) Run(ctx context.Context) error {
	// 1. Write PID file.
	if err := d.writePID(); err != nil {
		return err
	}
	defer d.removePID()

	// 2. Run migrations.
	migrator := store.NewMigrator(d.store)
	for _, m := range d.modules {
		m.RegisterSchema(migrator)
	}
	if err := migrator.Migrate(); err != nil {
		return fmt.Errorf("migrate: %w", err)
	}

	// 3. Register routes.
	for _, m := range d.modules {
		name := m.Name()
		d.router.RegisterModule(name, func(reg RouteRegistrar) {
			m.RegisterRoutes(reg)
		})
	}

	// 4. Start modules.
	for _, m := range d.modules {
		if err := m.OnStart(ctx); err != nil {
			return fmt.Errorf("start module %s: %w", m.Name(), err)
		}
	}

	// 5. Start IPC server. Initialize lastRequest so idle timeout works even with no requests.
	d.server = ipc.NewServer(d.cfg.SocketAddr, d.router)
	serverCtx, serverCancel := context.WithCancel(ctx)
	defer serverCancel()

	serverDone := make(chan error, 1)
	go func() { serverDone <- d.server.Serve(serverCtx) }()

	d.logger.Info("daemon started",
		"pid", os.Getpid(),
		"socket", d.cfg.SocketAddr,
	)

	// 6. Initialize idle timer baseline (so fresh daemon with no requests also times out).
	startTime := time.Now().Unix()

	// 7. Wait for shutdown signal or idle timeout.
	idleTicker := time.NewTicker(30 * time.Second)
	defer idleTicker.Stop()

	for {
		select {
		case <-ctx.Done():
			d.logger.Info("shutdown requested")
			goto shutdown
		case <-idleTicker.C:
			last := d.server.LastRequestTime()
			if last == 0 {
				last = startTime // no requests yet → use daemon start time
			}
			if time.Since(time.Unix(last, 0)) > d.cfg.IdleTimeout {
				d.logger.Info("idle timeout, shutting down")
				goto shutdown
			}
		}
	}

shutdown:
	serverCancel()
	<-serverDone

	// Stop modules in reverse order.
	for i := len(d.modules) - 1; i >= 0; i-- {
		d.modules[i].OnStop()
	}

	d.store.Close()
	return nil
}

func (d *Daemon) writePID() error {
	return os.WriteFile(d.cfg.PIDFilePath, []byte(strconv.Itoa(os.Getpid())), 0o644)
}

func (d *Daemon) removePID() {
	os.Remove(d.cfg.PIDFilePath)
}
```

**주의:** 위 코드에서 `RegisterModule` 클로저 내 `m` 변수 캡처에 주의. Go 1.22+에서는 루프 변수가 per-iteration 스코프이므로 안전. 이전 버전이면 `m := m` 필요.

- [ ] **Step 3: 테스트 통과 확인 + 커밋**

Run: `go test ./internal/daemon/... -v -race`
Expected: PASS

```bash
git add internal/daemon/daemon.go internal/daemon/daemon_test.go
git commit -m "feat(tools): BACKLOG-126 Daemon Core — 수명 관리, PID, 유휴 타임아웃"
```

---

## Task 10: CLI 프레임워크

**Files:**
- Create: `cmd/apex-agent/main.go` (수정), `internal/cli/root.go`, `daemon_cmd.go`

**Ref:** 스펙 §8 프로젝트 구조

- [ ] **Step 1: root.go 작성**

```go
// internal/cli/root.go
package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"
)

var Version = "dev"

func Execute() {
	root := &cobra.Command{
		Use:   "apex-agent",
		Short: "apex-agent: 개발 자동화 플랫폼",
		CompletionOptions: cobra.CompletionOptions{
			DisableDefaultCmd: true,
		},
	}

	root.AddCommand(daemonCmd())
	root.AddCommand(versionCmd())

	if err := root.Execute(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func versionCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "version",
		Short: "버전 출력",
		Run: func(cmd *cobra.Command, args []string) {
			fmt.Printf("apex-agent %s\n", Version)
		},
	}
}
```

- [ ] **Step 2: daemon_cmd.go 작성**

```go
// internal/cli/daemon_cmd.go
package cli

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

func daemonCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "daemon",
		Short: "데몬 관리",
	}

	cmd.AddCommand(daemonRunCmd())
	cmd.AddCommand(daemonStartCmd())
	cmd.AddCommand(daemonStopCmd())
	cmd.AddCommand(daemonStatusCmd())

	return cmd
}

// daemon run — foreground daemon (called by "start" internally).
func daemonRunCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "run",
		Short: "데몬 포그라운드 실행 (디버깅용)",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := platform.EnsureDataDir(); err != nil {
				return err
			}

			cfg := daemon.Config{
				DBPath:      platform.DBPath(),
				PIDFilePath: platform.PIDFilePath(),
				SocketAddr:  platform.SocketPath(),
				IdleTimeout: 30 * time.Minute,
			}

			d, err := daemon.New(cfg)
			if err != nil {
				return err
			}

			// Future: register concrete modules here.
			// d.Register(handoff.NewModule(d.Store()))
			// d.Register(queue.NewModule(d.Store()))

			ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
			defer cancel()

			return d.Run(ctx)
		},
	}
}

// daemon start — spawn detached daemon process.
func daemonStartCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "start",
		Short: "데몬 백그라운드 시작",
		RunE: func(cmd *cobra.Command, args []string) error {
			// Check if already running.
			if isRunning() {
				fmt.Println("daemon already running")
				return nil
			}

			// Spawn detached process.
			exe, _ := os.Executable()
			child := exec.Command(exe, "daemon", "run")
			detachProcess(child) // platform-specific

			if err := child.Start(); err != nil {
				return fmt.Errorf("start daemon: %w", err)
			}

			// Wait for readiness (poll IPC).
			addr := platform.SocketPath()
			for i := 0; i < 60; i++ { // 60 * 50ms = 3s max
				time.Sleep(50 * time.Millisecond)
				conn, err := ipc.Dial(addr)
				if err == nil {
					conn.Close()
					fmt.Printf("daemon started (pid %d)\n", child.Process.Pid)
					return nil
				}
			}

			return fmt.Errorf("daemon failed to start within 3 seconds")
		},
	}
}

// daemon stop — send shutdown signal.
func daemonStopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "데몬 종료",
		RunE: func(cmd *cobra.Command, args []string) error {
			pid, err := readPID()
			if err != nil {
				return fmt.Errorf("daemon not running")
			}
			proc, err := os.FindProcess(pid)
			if err != nil {
				return err
			}
			if runtime.GOOS == "windows" {
				proc.Kill() // Windows doesn't support SIGTERM gracefully
			} else {
				proc.Signal(syscall.SIGTERM)
			}
			fmt.Println("daemon stopped")
			return nil
		},
	}
}

// daemon status — check if running.
func daemonStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "데몬 상태 확인",
		Run: func(cmd *cobra.Command, args []string) {
			if isRunning() {
				pid, _ := readPID()
				fmt.Printf("daemon running (pid %d)\n", pid)
			} else {
				fmt.Println("daemon not running")
			}
		},
	}
}

func isRunning() bool {
	pid, err := readPID()
	if err != nil {
		return false
	}
	return platform.IsProcessAlive(pid)
}

func readPID() (int, error) {
	data, err := os.ReadFile(platform.PIDFilePath())
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}

// detachProcess: 플랫폼별 빌드 태그 파일에서 구현.
// internal/cli/detach_unix.go, detach_windows.go 참조.
```

별도 파일로 분리 (런타임 체크가 아닌 컴파일 타임 분리):

```go
// internal/cli/detach_unix.go
//go:build !windows

package cli

import (
	"os/exec"
	"syscall"
)

func detachProcess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
}
```

```go
// internal/cli/detach_windows.go
//go:build windows

package cli

import (
	"os/exec"
	"syscall"
)

func detachProcess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP,
	}
}
```

- [ ] **Step 3: main.go 업데이트**

```go
// cmd/apex-agent/main.go
package main

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cli"

func main() {
	cli.Execute()
}
```

- [ ] **Step 4: 빌드 + 수동 테스트**

```bash
go build -o apex-agent ./cmd/apex-agent
./apex-agent version         # → "apex-agent dev"
./apex-agent daemon start    # → daemon started (pid XXXX)
./apex-agent daemon status   # → daemon running (pid XXXX)
./apex-agent daemon stop     # → daemon stopped
./apex-agent daemon status   # → daemon not running
```

- [ ] **Step 5: 커밋**

```bash
git add cmd/ internal/cli/
git commit -m "feat(tools): BACKLOG-126 CLI 프레임워크 — cobra root + daemon 커맨드"
```

---

## Task 11: CI Integration

**Files:**
- Modify: `.github/workflows/ci.yml`

**Ref:** 스펙 §9 테스트 전략

- [ ] **Step 1: ci.yml에 Go 빌드+테스트 스텝 추가**

기존 C++ 빌드 스텝 이후에 추가:

```yaml
  go-test:
    name: Go (apex-agent)
    runs-on: ubuntu-latest
    defaults:
      run:
        working-directory: apex_tools/apex-agent
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - run: go test ./... -race -cover -v
      - run: go vet ./...
      - name: Build (linux)
        run: go build -o apex-agent ./cmd/apex-agent
      - name: Build (windows cross-compile)
        run: GOOS=windows GOARCH=amd64 go build -o apex-agent.exe ./cmd/apex-agent
```

- [ ] **Step 2: 커밋**

```bash
git add .github/workflows/ci.yml
git commit -m "ci(tools): BACKLOG-126 Go 빌드+테스트 파이프라인 추가"
```

---

## Task 12: E2E Smoke Test

**Files:**
- Create: `e2e_test.go`

Phase 0 검증 목표: **데몬 기동 → 빈 모듈 등록 → IPC 왕복 → 데몬 종료**.

- [ ] **Step 1: E2E 테스트 작성**

```go
// e2e/e2e_test.go (별도 디렉토리 — go test -tags e2e ./e2e/... 로 실행)
//go:build e2e

package e2e

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testAddr() string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-e2e`
	}
	return "/tmp/apex-agent-e2e.sock"
}

// echoModule is a minimal test module.
type echoModule struct{}

func (m *echoModule) Name() string { return "echo" }
func (m *echoModule) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("ping", func(ctx context.Context, params json.RawMessage, ws string) (any, error) {
		return map[string]string{"status": "ok", "workspace": ws}, nil
	})
}
func (m *echoModule) RegisterSchema(mig *store.Migrator)  {}
func (m *echoModule) OnStart(ctx context.Context) error    { return nil }
func (m *echoModule) OnStop() error                        { return nil }

func TestE2E_FullRoundtrip(t *testing.T) {
	tmpDir := t.TempDir()
	addr := testAddr()

	cfg := daemon.Config{
		DBPath:      filepath.Join(tmpDir, "e2e.db"),
		PIDFilePath: filepath.Join(tmpDir, "e2e.pid"),
		SocketAddr:  addr,
		IdleTimeout: 5 * time.Minute,
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	d.Register(&echoModule{})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	// Wait for readiness.
	time.Sleep(200 * time.Millisecond)

	// PID file exists.
	if _, err := os.Stat(cfg.PIDFilePath); os.IsNotExist(err) {
		t.Fatal("PID file not found")
	}

	// Send request via client.
	client := ipc.NewClient(addr)
	resp, err := client.Send(ctx, "echo", "ping", nil, "branch_02")
	if err != nil {
		t.Fatalf("client.Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("response not OK: %s", resp.Error)
	}

	var data map[string]string
	json.Unmarshal(resp.Data, &data)
	if data["status"] != "ok" || data["workspace"] != "branch_02" {
		t.Errorf("unexpected response: %v", data)
	}

	// Unknown module returns error.
	resp2, _ := client.Send(ctx, "nonexistent", "action", nil, "ws")
	if resp2.OK {
		t.Error("expected error for unknown module")
	}

	// Shutdown.
	cancel()
	if err := <-done; err != nil {
		t.Errorf("daemon error: %v", err)
	}

	// PID cleaned up.
	if _, err := os.Stat(cfg.PIDFilePath); !os.IsNotExist(err) {
		t.Error("PID file not cleaned up after shutdown")
	}
}
```

- [ ] **Step 2: E2E 테스트 실행**

Run: `go test -tags e2e -v -race ./e2e/...`
Expected: PASS — 데몬 기동 → echo 모듈 응답 → 종료 성공

- [ ] **Step 3: 커밋**

```bash
git add e2e/
git commit -m "test(tools): BACKLOG-126 Phase 0 E2E 스모크 테스트"
```

**참고:** `go test ./... -race -cover` (일반 CI)에서는 e2e 빌드 태그가 없으므로 E2E 테스트가 자동 스킵된다. E2E 실행은 `go test -tags e2e ./e2e/...`로 명시적으로.

---

## 완료 기준

Phase 0 완료 시 다음이 동작해야 한다:

1. `go build ./cmd/apex-agent` — 바이너리 빌드 성공
2. `go test ./... -race -cover` — 모든 단위/통합 테스트 PASS
3. `go test -tags e2e -v` — E2E 스모크 테스트 PASS
4. `apex-agent daemon start/stop/status` — 데몬 수명 관리 동작
5. CLI → Named Pipe/Unix Socket → 데몬 → Router → Module → 응답 전체 경로 동작
6. SQLite WAL 모드 활성, Migrator 동작 확인
7. CI에서 Go 빌드+테스트 통과
