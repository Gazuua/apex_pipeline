# Session Server (Phase 2-3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ConPTY 기반 독립 프로세스 세션 서버 + 대시보드 /branches 페이지로 멀티브랜치 Claude Code 세션을 웹 터미널로 제어한다.

**Architecture:** 같은 Go 바이너리(`apex-agent`)에서 `daemon run`과 `session run` 두 서브커맨드가 각각 독립 프로세스로 실행. 세션 서버(:7601)가 ConPTY를 관리하고 WebSocket으로 터미널 I/O를 스트리밍. 대시보드(:7600)가 리버스 프록시로 세션 서버를 통합. SQLite WAL 모드로 DB를 공유.

**Tech Stack:** Go 1.26, ConPTY (Windows API via golang.org/x/sys/windows), gorilla/websocket, xterm.js v5 (CDN), HTMX

**Design Spec:** `docs/apex_tools/plans/20260326_152349_workspace_session_mgmt.md`

---

## File Structure

### Phase 2 — Session Server Core

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `internal/session/terminal.go` | Terminal 인터페이스 + output ring buffer |
| Create | `internal/session/conpty_windows.go` | Windows ConPTY 구현 (build tag: windows) |
| Create | `internal/session/conpty_stub.go` | Unix stub — 빌드만 통과 (build tag: !windows) |
| Create | `internal/session/manager.go` | 세션 생명주기 관리 (create/stop/list) + DB 연동 |
| Create | `internal/session/websocket.go` | WebSocket 핸들러 (xterm.js ↔ ConPTY I/O) |
| Create | `internal/session/server.go` | HTTP + WS 서버 (:7601) |
| Create | `internal/session/watchdog.go` | 프로세스 사망 감지 + DB 상태 갱신 |
| Create | `internal/cli/session_cmd.go` | CLI: session run/start/stop/status/send |
| Modify | `internal/cli/root.go:23` | `root.AddCommand(sessionCmd())` 추가 |
| Modify | `internal/platform/paths.go` | `SessionPIDFilePath()`, `SessionLogDir()` 추가 |
| Modify | `go.mod` | `github.com/gorilla/websocket` 의존성 추가 |

### Phase 3 — Dashboard Extensions

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `internal/httpd/proxy.go` | 리버스 프록시 (/api/session/*, /ws/session/* → :7601) |
| Create | `internal/httpd/branches.go` | /branches 페이지 + HTMX 파셜 핸들러 |
| Create | `internal/httpd/templates/branches.html` | Branches 페이지 템플릿 (xterm.js 터미널) |
| Create | `internal/httpd/templates/partials/branches.html` | 브랜치 목록 파셜 |
| Create | `internal/httpd/templates/partials/blocked_badge.html` | ⚠ 뱃지 파셜 |
| Create | `internal/httpd/static/terminal.js` | xterm.js WebSocket 연결 + 터미널 관리 |
| Modify | `internal/httpd/server.go` | WorkspaceQuerier 인터페이스 + Server 필드 추가 |
| Modify | `internal/httpd/routes.go` | /branches, 파셜, API, 프록시 라우트 등록 |
| Modify | `internal/httpd/render.go` | pageFiles에 "branches" 추가 |
| Modify | `internal/httpd/templates/layout.html` | nav에 Branches 링크 + ⚠ 뱃지 추가 |
| Modify | `internal/cli/daemon_cmd.go` | WorkspaceQuerier 어댑터 + httpd.New 파라미터 확장 |

### Tests

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `internal/session/manager_test.go` | Manager 유닛 테스트 (mock terminal) |
| Create | `internal/session/terminal_test.go` | Ring buffer 테스트 |
| Create | `e2e/session_server_test.go` | 세션 서버 E2E (start/stop/status API) |

---

## Task 1: Dependencies & Platform Paths

**Files:**
- Modify: `go.mod`
- Modify: `internal/platform/paths.go:19-21`

- [ ] **Step 1: gorilla/websocket 의존성 추가**

```bash
cd apex_tools/apex-agent && go get github.com/gorilla/websocket@latest
```

- [ ] **Step 2: platform paths에 세션 관련 경로 추가**

`internal/platform/paths.go` — 상수 블록에 추가:

```go
const (
	AppName              = "apex-agent"
	pipeName             = `\\.\pipe\apex-agent`
	socketName           = "apex-agent.sock"
	dbName               = "apex-agent.db"
	pidName              = "apex-agent.pid"
	sessionPIDName       = "apex-session.pid"
	sessionLogDirName    = "sessions"
	maintenanceName      = "apex-agent.maintenance"
)
```

함수 추가:

```go
// SessionPIDFilePath returns the full path to the session server PID file.
func SessionPIDFilePath() string { return filepath.Join(DataDir(), sessionPIDName) }

// SessionLogDir returns the directory for session log files.
// Falls back to DataDir()/sessions if config log_dir is empty.
func SessionLogDir(configDir string) string {
	if configDir != "" {
		return configDir
	}
	return filepath.Join(DataDir(), sessionLogDirName)
}
```

- [ ] **Step 3: go mod tidy 실행**

```bash
cd apex_tools/apex-agent && go mod tidy
```

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/go.mod apex_tools/apex-agent/go.sum apex_tools/apex-agent/internal/platform/paths.go
git commit -m "feat(tools): session 서버 의존성 + platform 경로 추가"
```

---

## Task 2: Terminal Interface & Ring Buffer

**Files:**
- Create: `internal/session/terminal.go`
- Create: `internal/session/terminal_test.go`

- [ ] **Step 1: terminal_test.go — ring buffer 테스트 작성**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"testing"
)

func TestRingBuffer_Basic(t *testing.T) {
	rb := NewRingBuffer(3)
	rb.Write([]byte("line1\nline2\nline3\n"))

	lines := rb.Lines()
	if len(lines) != 3 {
		t.Fatalf("expected 3 lines, got %d", len(lines))
	}
	if lines[0] != "line1\n" || lines[1] != "line2\n" || lines[2] != "line3\n" {
		t.Errorf("unexpected lines: %v", lines)
	}
}

func TestRingBuffer_Overflow(t *testing.T) {
	rb := NewRingBuffer(2)
	rb.Write([]byte("a\nb\nc\n"))

	lines := rb.Lines()
	if len(lines) != 2 {
		t.Fatalf("expected 2 lines, got %d", len(lines))
	}
	// Oldest line dropped
	if lines[0] != "b\n" || lines[1] != "c\n" {
		t.Errorf("expected [b\\n, c\\n], got %v", lines)
	}
}

func TestRingBuffer_PartialLine(t *testing.T) {
	rb := NewRingBuffer(10)
	rb.Write([]byte("partial"))
	rb.Write([]byte(" line\nfull\n"))

	lines := rb.Lines()
	if len(lines) != 2 {
		t.Fatalf("expected 2 lines, got %d", len(lines))
	}
	if lines[0] != "partial line\n" {
		t.Errorf("expected merged partial, got %q", lines[0])
	}
}

func TestRingBuffer_Snapshot(t *testing.T) {
	rb := NewRingBuffer(5)
	rb.Write([]byte("hello\nworld\n"))

	snap := rb.Snapshot()
	if string(snap) != "hello\nworld\n" {
		t.Errorf("snapshot = %q, want %q", snap, "hello\nworld\n")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

```bash
cd apex_tools/apex-agent && go test ./internal/session/... -run TestRingBuffer -v
```

Expected: 컴파일 에러 (패키지/타입 없음)

- [ ] **Step 3: terminal.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"bytes"
	"io"
	"sync"
)

// Terminal is the interface for a pseudo-terminal process.
// ConPTY implements this on Windows; Unix uses a stub.
type Terminal interface {
	io.ReadWriteCloser
	// Resize changes the terminal dimensions.
	Resize(cols, rows int) error
	// Pid returns the process ID of the child process.
	Pid() int
}

// RingBuffer stores the last N lines of terminal output for replay on reconnect.
type RingBuffer struct {
	mu      sync.Mutex
	lines   []string
	maxLines int
	pos     int
	full    bool
	partial string // incomplete line (no trailing newline yet)
}

// NewRingBuffer creates a ring buffer that keeps at most maxLines lines.
func NewRingBuffer(maxLines int) *RingBuffer {
	return &RingBuffer{
		lines:    make([]string, maxLines),
		maxLines: maxLines,
	}
}

// Write appends data to the buffer, splitting on newlines.
func (rb *RingBuffer) Write(data []byte) {
	rb.mu.Lock()
	defer rb.mu.Unlock()

	// Prepend any partial line from the previous write.
	buf := rb.partial + string(data)
	rb.partial = ""

	for {
		idx := bytes.IndexByte([]byte(buf), '\n')
		if idx < 0 {
			// No more newlines — save as partial.
			rb.partial = buf
			return
		}
		line := buf[:idx+1]
		buf = buf[idx+1:]

		rb.lines[rb.pos] = line
		rb.pos = (rb.pos + 1) % rb.maxLines
		if rb.pos == 0 {
			rb.full = true
		}
	}
}

// Lines returns all stored lines in chronological order.
func (rb *RingBuffer) Lines() []string {
	rb.mu.Lock()
	defer rb.mu.Unlock()

	if !rb.full {
		result := make([]string, rb.pos)
		copy(result, rb.lines[:rb.pos])
		return result
	}
	result := make([]string, rb.maxLines)
	copy(result, rb.lines[rb.pos:])
	copy(result[rb.maxLines-rb.pos:], rb.lines[:rb.pos])
	return result
}

// Snapshot returns all stored lines concatenated as a single byte slice.
func (rb *RingBuffer) Snapshot() []byte {
	lines := rb.Lines()
	var buf bytes.Buffer
	for _, l := range lines {
		buf.WriteString(l)
	}
	return buf.Bytes()
}
```

- [ ] **Step 4: 테스트 통과 확인**

```bash
cd apex_tools/apex-agent && go test ./internal/session/... -run TestRingBuffer -v
```

Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/
git commit -m "feat(tools): Terminal 인터페이스 + RingBuffer 구현"
```

---

## Task 3: ConPTY Implementation (Windows) & Unix Stub

**Files:**
- Create: `internal/session/conpty_windows.go`
- Create: `internal/session/conpty_stub.go`

- [ ] **Step 1: conpty_windows.go — Windows ConPTY 래퍼**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build windows

package session

import (
	"fmt"
	"os"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	kernel32                      = windows.NewLazySystemDLL("kernel32.dll")
	procCreatePseudoConsole       = kernel32.NewProc("CreatePseudoConsole")
	procResizePseudoConsole       = kernel32.NewProc("ResizePseudoConsole")
	procClosePseudoConsole        = kernel32.NewProc("ClosePseudoConsole")
	procInitializeProcThreadAttrs = kernel32.NewProc("InitializeProcThreadAttributeList")
	procUpdateProcThreadAttr      = kernel32.NewProc("UpdateProcThreadAttribute")
	procDeleteProcThreadAttrs     = kernel32.NewProc("DeleteProcThreadAttributeList")
)

const _PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016

type conPTYSize struct {
	X, Y int16
}

// ConPTY wraps a Windows pseudo-console attached to a child process.
type ConPTY struct {
	hPC           windows.Handle
	pipeIn        *os.File // write end → ConPTY stdin
	pipeOut       *os.File // read end ← ConPTY stdout
	process       *os.Process
	closeOnce     sync.Once
}

// NewConPTY creates a ConPTY with the given dimensions and spawns cmdLine inside it.
func NewConPTY(cmdLine string, cols, rows int) (*ConPTY, error) {
	// Create pipes for ConPTY I/O.
	ptyInR, ptyInW, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("create input pipe: %w", err)
	}
	ptyOutR, ptyOutW, err := os.Pipe()
	if err != nil {
		ptyInR.Close()
		ptyInW.Close()
		return nil, fmt.Errorf("create output pipe: %w", err)
	}

	// CreatePseudoConsole
	size := conPTYSize{X: int16(cols), Y: int16(rows)}
	var hPC windows.Handle
	r, _, _ := procCreatePseudoConsole.Call(
		uintptr(*(*uint32)(unsafe.Pointer(&size))),
		ptyInR.Fd(),
		ptyOutW.Fd(),
		0,
		uintptr(unsafe.Pointer(&hPC)),
	)
	if r != 0 {
		ptyInR.Close()
		ptyInW.Close()
		ptyOutR.Close()
		ptyOutW.Close()
		return nil, fmt.Errorf("CreatePseudoConsole: HRESULT 0x%08x", r)
	}

	// Close the pipe ends that the ConPTY now owns.
	ptyInR.Close()
	ptyOutW.Close()

	// Spawn process with ConPTY attached via PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
	proc, err := spawnInConPTY(cmdLine, hPC)
	if err != nil {
		procClosePseudoConsole.Call(uintptr(hPC))
		ptyInW.Close()
		ptyOutR.Close()
		return nil, fmt.Errorf("spawn process: %w", err)
	}

	return &ConPTY{
		hPC:     hPC,
		pipeIn:  ptyInW,
		pipeOut: ptyOutR,
		process: proc,
	}, nil
}

func spawnInConPTY(cmdLine string, hPC windows.Handle) (*os.Process, error) {
	// Initialize thread attribute list.
	var size uintptr
	procInitializeProcThreadAttrs.Call(0, 1, 0, uintptr(unsafe.Pointer(&size)))
	attrList := make([]byte, size)
	r, _, _ := procInitializeProcThreadAttrs.Call(
		uintptr(unsafe.Pointer(&attrList[0])),
		1, 0, uintptr(unsafe.Pointer(&size)),
	)
	if r == 0 {
		return nil, fmt.Errorf("InitializeProcThreadAttributeList failed")
	}
	defer procDeleteProcThreadAttrs.Call(uintptr(unsafe.Pointer(&attrList[0])))

	// Set pseudo console attribute.
	r, _, _ = procUpdateProcThreadAttr.Call(
		uintptr(unsafe.Pointer(&attrList[0])),
		0,
		_PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
		uintptr(hPC),
		unsafe.Sizeof(hPC),
		0, 0,
	)
	if r == 0 {
		return nil, fmt.Errorf("UpdateProcThreadAttribute failed")
	}

	// CreateProcess with EXTENDED_STARTUPINFO_PRESENT.
	cmdLinePtr, _ := windows.UTF16PtrFromString(cmdLine)
	si := windows.StartupInfoEx{
		StartupInfo:            windows.StartupInfo{Cb: uint32(unsafe.Sizeof(windows.StartupInfoEx{}))},
		ProcThreadAttributeList: (*windows.ProcThreadAttributeList)(unsafe.Pointer(&attrList[0])),
	}
	var pi windows.ProcessInformation
	err := windows.CreateProcess(
		nil, cmdLinePtr, nil, nil, false,
		windows.EXTENDED_STARTUPINFO_PRESENT, nil, nil,
		&si.StartupInfo, &pi,
	)
	if err != nil {
		return nil, fmt.Errorf("CreateProcess: %w", err)
	}
	windows.CloseHandle(pi.Thread)

	proc, _ := os.FindProcess(int(pi.ProcessId))
	return proc, nil
}

func (c *ConPTY) Read(p []byte) (int, error) {
	return c.pipeOut.Read(p)
}

func (c *ConPTY) Write(p []byte) (int, error) {
	return c.pipeIn.Write(p)
}

func (c *ConPTY) Close() error {
	var firstErr error
	c.closeOnce.Do(func() {
		procClosePseudoConsole.Call(uintptr(c.hPC))
		if err := c.pipeIn.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
		if err := c.pipeOut.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
		if c.process != nil {
			c.process.Kill()
			c.process.Wait()
		}
	})
	return firstErr
}

func (c *ConPTY) Resize(cols, rows int) error {
	size := conPTYSize{X: int16(cols), Y: int16(rows)}
	r, _, _ := procResizePseudoConsole.Call(
		uintptr(c.hPC),
		uintptr(*(*uint32)(unsafe.Pointer(&size))),
	)
	if r != 0 {
		return fmt.Errorf("ResizePseudoConsole: HRESULT 0x%08x", r)
	}
	return nil
}

func (c *ConPTY) Pid() int {
	if c.process != nil {
		return c.process.Pid
	}
	return 0
}
```

- [ ] **Step 2: conpty_stub.go — Unix stub**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build !windows

package session

import "fmt"

// ConPTY is not supported on non-Windows platforms.
type ConPTY struct{}

func NewConPTY(cmdLine string, cols, rows int) (*ConPTY, error) {
	return nil, fmt.Errorf("ConPTY is only available on Windows")
}

func (c *ConPTY) Read(p []byte) (int, error)  { return 0, fmt.Errorf("not supported") }
func (c *ConPTY) Write(p []byte) (int, error) { return 0, fmt.Errorf("not supported") }
func (c *ConPTY) Close() error                { return nil }
func (c *ConPTY) Resize(cols, rows int) error  { return fmt.Errorf("not supported") }
func (c *ConPTY) Pid() int                    { return 0 }
```

- [ ] **Step 3: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./internal/session/...
```

Expected: 성공

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/conpty_windows.go apex_tools/apex-agent/internal/session/conpty_stub.go
git commit -m "feat(tools): ConPTY Windows 구현 + Unix stub"
```

---

## Task 4: Session Manager

**Files:**
- Create: `internal/session/manager.go`
- Create: `internal/session/manager_test.go`

- [ ] **Step 1: manager_test.go 작성**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"io"
	"sync"
	"testing"
)

// mockTerminal implements Terminal for testing without ConPTY.
type mockTerminal struct {
	mu     sync.Mutex
	buf    []byte
	closed bool
	pid    int
}

func newMockTerminal(pid int) *mockTerminal {
	return &mockTerminal{pid: pid}
}

func (m *mockTerminal) Read(p []byte) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.closed {
		return 0, io.EOF
	}
	if len(m.buf) == 0 {
		return 0, io.EOF
	}
	n := copy(p, m.buf)
	m.buf = m.buf[n:]
	return n, nil
}

func (m *mockTerminal) Write(p []byte) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.closed {
		return 0, io.ErrClosedPipe
	}
	m.buf = append(m.buf, p...)
	return len(p), nil
}

func (m *mockTerminal) Close() error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.closed = true
	return nil
}

func (m *mockTerminal) Resize(cols, rows int) error { return nil }
func (m *mockTerminal) Pid() int                    { return m.pid }

func TestManager_CreateAndStop(t *testing.T) {
	mgr := NewManager(ManagerConfig{
		OutputBufferLines: 100,
		LogDir:            t.TempDir(),
	})

	term := newMockTerminal(12345)
	sess, err := mgr.Register("test_ws", "test-session-id", term)
	if err != nil {
		t.Fatalf("Register: %v", err)
	}
	if sess.WorkspaceID != "test_ws" {
		t.Errorf("workspace = %q, want %q", sess.WorkspaceID, "test_ws")
	}
	if sess.Status != StatusManaged {
		t.Errorf("status = %q, want %q", sess.Status, StatusManaged)
	}

	info := mgr.Get("test_ws")
	if info == nil {
		t.Fatal("Get returned nil")
	}

	err = mgr.Stop(context.Background(), "test_ws")
	if err != nil {
		t.Fatalf("Stop: %v", err)
	}

	info = mgr.Get("test_ws")
	if info != nil {
		t.Error("expected nil after stop")
	}
}

func TestManager_List(t *testing.T) {
	mgr := NewManager(ManagerConfig{
		OutputBufferLines: 100,
		LogDir:            t.TempDir(),
	})

	mgr.Register("ws_a", "id-a", newMockTerminal(1))
	mgr.Register("ws_b", "id-b", newMockTerminal(2))

	list := mgr.List()
	if len(list) != 2 {
		t.Fatalf("expected 2 sessions, got %d", len(list))
	}
}

func TestManager_DuplicateRegister(t *testing.T) {
	mgr := NewManager(ManagerConfig{
		OutputBufferLines: 100,
		LogDir:            t.TempDir(),
	})

	mgr.Register("ws_dup", "id-1", newMockTerminal(1))
	_, err := mgr.Register("ws_dup", "id-2", newMockTerminal(2))
	if err == nil {
		t.Error("expected error on duplicate register")
	}
}
```

- [ ] **Step 2: 테스트 실패 확인**

```bash
cd apex_tools/apex-agent && go test ./internal/session/... -run TestManager -v
```

Expected: 컴파일 에러

- [ ] **Step 3: manager.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
)

var ml = log.WithModule("session")

const (
	StatusStop     = "STOP"
	StatusManaged  = "MANAGED"
	StatusExternal = "EXTERNAL"
)

// SessionInfo holds runtime state of an active session.
type SessionInfo struct {
	WorkspaceID string    `json:"workspace_id"`
	SessionID   string    `json:"session_id"`
	Status      string    `json:"status"`
	PID         int       `json:"pid"`
	StartedAt   time.Time `json:"started_at"`
	LogPath     string    `json:"log_path"`

	term    Terminal
	ring    *RingBuffer
	logFile *os.File
	clients map[chan []byte]struct{} // WebSocket subscribers
	mu      sync.Mutex
	done    chan struct{} // closed when read pump exits
}

// ManagerConfig holds session manager settings.
type ManagerConfig struct {
	OutputBufferLines int
	LogDir            string
}

// Manager manages active terminal sessions.
type Manager struct {
	cfg      ManagerConfig
	sessions map[string]*SessionInfo // keyed by workspace_id
	mu       sync.RWMutex
}

// NewManager creates a session Manager.
func NewManager(cfg ManagerConfig) *Manager {
	if cfg.OutputBufferLines <= 0 {
		cfg.OutputBufferLines = 500
	}
	return &Manager{
		cfg:      cfg,
		sessions: make(map[string]*SessionInfo),
	}
}

// Register adds an already-created terminal to the manager.
// Starts the read pump that broadcasts output to WebSocket clients and log file.
func (m *Manager) Register(workspaceID, sessionID string, term Terminal) (*SessionInfo, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if _, exists := m.sessions[workspaceID]; exists {
		return nil, fmt.Errorf("session already active for %s", workspaceID)
	}

	// Ensure log directory exists.
	logDir := filepath.Join(m.cfg.LogDir, workspaceID)
	os.MkdirAll(logDir, 0o755)
	logPath := filepath.Join(logDir, time.Now().Format("20060102_150405")+".log")
	logFile, err := os.Create(logPath)
	if err != nil {
		ml.Warn("session log file creation failed", "path", logPath, "err", err)
		// Non-fatal — session works without log file.
	}

	info := &SessionInfo{
		WorkspaceID: workspaceID,
		SessionID:   sessionID,
		Status:      StatusManaged,
		PID:         term.Pid(),
		StartedAt:   time.Now(),
		LogPath:     logPath,
		term:        term,
		ring:        NewRingBuffer(m.cfg.OutputBufferLines),
		logFile:     logFile,
		clients:     make(map[chan []byte]struct{}),
		done:        make(chan struct{}),
	}

	m.sessions[workspaceID] = info
	go info.readPump()

	ml.Info("session registered", "workspace", workspaceID, "session_id", sessionID, "pid", term.Pid())
	return info, nil
}

// Get returns session info for a workspace, or nil if not active.
func (m *Manager) Get(workspaceID string) *SessionInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return m.sessions[workspaceID]
}

// List returns all active sessions.
func (m *Manager) List() []*SessionInfo {
	m.mu.RLock()
	defer m.mu.RUnlock()
	result := make([]*SessionInfo, 0, len(m.sessions))
	for _, s := range m.sessions {
		result = append(result, s)
	}
	return result
}

// Stop terminates a session and removes it from the manager.
func (m *Manager) Stop(_ context.Context, workspaceID string) error {
	m.mu.Lock()
	info, exists := m.sessions[workspaceID]
	if !exists {
		m.mu.Unlock()
		return fmt.Errorf("no active session for %s", workspaceID)
	}
	delete(m.sessions, workspaceID)
	m.mu.Unlock()

	// Close terminal — this will cause the read pump to exit.
	if err := info.term.Close(); err != nil {
		ml.Warn("terminal close error", "workspace", workspaceID, "err", err)
	}

	// Wait for read pump to finish.
	<-info.done

	if info.logFile != nil {
		info.logFile.Close()
	}

	// Close all subscriber channels.
	info.mu.Lock()
	for ch := range info.clients {
		close(ch)
		delete(info.clients, ch)
	}
	info.mu.Unlock()

	ml.Info("session stopped", "workspace", workspaceID)
	return nil
}

// StopAll terminates all active sessions. Called during graceful shutdown.
func (m *Manager) StopAll(ctx context.Context) {
	m.mu.RLock()
	ids := make([]string, 0, len(m.sessions))
	for id := range m.sessions {
		ids = append(ids, id)
	}
	m.mu.RUnlock()

	for _, id := range ids {
		if err := m.Stop(ctx, id); err != nil {
			ml.Warn("stop all: session stop failed", "workspace", id, "err", err)
		}
	}
}

// Remove removes a dead session from tracking (called by watchdog).
func (m *Manager) Remove(workspaceID string) {
	m.mu.Lock()
	info, exists := m.sessions[workspaceID]
	if exists {
		delete(m.sessions, workspaceID)
	}
	m.mu.Unlock()

	if exists && info.logFile != nil {
		info.logFile.Close()
	}
}

// Subscribe registers a WebSocket client for output broadcast.
// Returns a channel that receives terminal output and a cleanup function.
func (s *SessionInfo) Subscribe() (<-chan []byte, func()) {
	ch := make(chan []byte, 256)
	s.mu.Lock()
	s.clients[ch] = struct{}{}
	s.mu.Unlock()

	return ch, func() {
		s.mu.Lock()
		delete(s.clients, ch)
		s.mu.Unlock()
	}
}

// SendInput writes data to the terminal's stdin.
func (s *SessionInfo) SendInput(data []byte) error {
	_, err := s.term.Write(data)
	return err
}

// readPump reads from the terminal and broadcasts to all subscribers + log file.
func (s *SessionInfo) readPump() {
	defer close(s.done)
	buf := make([]byte, 4096)
	for {
		n, err := s.term.Read(buf)
		if n > 0 {
			data := make([]byte, n)
			copy(data, buf[:n])

			// Ring buffer for replay.
			s.ring.Write(data)

			// Log file.
			if s.logFile != nil {
				s.logFile.Write(data)
			}

			// Broadcast to WebSocket clients.
			s.mu.Lock()
			for ch := range s.clients {
				select {
				case ch <- data:
				default:
					// Slow client — drop frame.
				}
			}
			s.mu.Unlock()
		}
		if err != nil {
			if err != io.EOF {
				ml.Debug("terminal read ended", "workspace", s.WorkspaceID, "err", err)
			}
			return
		}
	}
}
```

- [ ] **Step 4: 테스트 통과 확인**

```bash
cd apex_tools/apex-agent && go test ./internal/session/... -run TestManager -v
```

Expected: PASS

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/manager.go apex_tools/apex-agent/internal/session/manager_test.go
git commit -m "feat(tools): session Manager — 세션 생명주기 + 출력 브로드캐스트"
```

---

## Task 5: WebSocket Handler

**Files:**
- Create: `internal/session/websocket.go`

- [ ] **Step 1: websocket.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"net/http"

	"github.com/gorilla/websocket"
)

var wsUpgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	CheckOrigin:     func(r *http.Request) bool { return true }, // localhost only
}

// HandleWebSocket upgrades an HTTP connection to WebSocket and bridges it
// to a terminal session's I/O. Binary frames carry raw terminal bytes.
func HandleWebSocket(mgr *Manager, w http.ResponseWriter, r *http.Request, workspaceID string) {
	info := mgr.Get(workspaceID)
	if info == nil {
		http.Error(w, "no active session for "+workspaceID, http.StatusNotFound)
		return
	}

	conn, err := wsUpgrader.Upgrade(w, r, nil)
	if err != nil {
		ml.Warn("websocket upgrade failed", "workspace", workspaceID, "err", err)
		return
	}
	defer conn.Close()

	// Replay buffered output on connect.
	snap := info.ring.Snapshot()
	if len(snap) > 0 {
		if err := conn.WriteMessage(websocket.BinaryMessage, snap); err != nil {
			return
		}
	}

	// Subscribe to live output.
	ch, unsub := info.Subscribe()
	defer unsub()

	// Write pump: terminal output → WebSocket.
	done := make(chan struct{})
	go func() {
		defer close(done)
		for data := range ch {
			if err := conn.WriteMessage(websocket.BinaryMessage, data); err != nil {
				return
			}
		}
	}()

	// Read pump: WebSocket → terminal stdin.
	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if err := info.SendInput(msg); err != nil {
			ml.Debug("stdin write failed", "workspace", workspaceID, "err", err)
			break
		}
	}

	// Wait for write pump to finish.
	unsub()
	<-done
}
```

- [ ] **Step 2: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./internal/session/...
```

Expected: 성공

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/websocket.go
git commit -m "feat(tools): WebSocket 핸들러 — xterm.js ↔ ConPTY 브리지"
```

---

## Task 6: Watchdog

**Files:**
- Create: `internal/session/watchdog.go`

- [ ] **Step 1: watchdog.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// DBUpdater is called by the watchdog to update session state in the DB.
// Avoids direct store dependency — caller provides the callback.
type DBUpdater func(workspaceID string, status string, pid int, sessionID string, logPath string)

// Watchdog periodically checks if managed processes are still alive.
// Dead sessions are removed from the Manager and their DB state is reset to STOP.
type Watchdog struct {
	mgr      *Manager
	interval time.Duration
	onUpdate DBUpdater
}

// NewWatchdog creates a watchdog that checks at the given interval.
func NewWatchdog(mgr *Manager, interval time.Duration, onUpdate DBUpdater) *Watchdog {
	return &Watchdog{
		mgr:      mgr,
		interval: interval,
		onUpdate: onUpdate,
	}
}

// Run starts the watchdog loop. Blocks until ctx is canceled.
func (w *Watchdog) Run(ctx context.Context) {
	ticker := time.NewTicker(w.interval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			w.check()
		}
	}
}

func (w *Watchdog) check() {
	sessions := w.mgr.List()
	for _, s := range sessions {
		if s.PID <= 0 {
			continue
		}
		if !platform.IsProcessAlive(s.PID) {
			ml.Info("session process dead, cleaning up", "workspace", s.WorkspaceID, "pid", s.PID)
			w.mgr.Remove(s.WorkspaceID)
			if w.onUpdate != nil {
				w.onUpdate(s.WorkspaceID, StatusStop, 0, "", "")
			}
		}
	}
}
```

- [ ] **Step 2: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./internal/session/...
```

Expected: 성공

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/watchdog.go
git commit -m "feat(tools): Watchdog — 세션 프로세스 사망 감지 + DB 상태 리셋"
```

---

## Task 7: Session HTTP Server

**Files:**
- Create: `internal/session/server.go`

- [ ] **Step 1: server.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/workspace"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Server is the standalone session server process (:7601).
type Server struct {
	cfg     config.SessionConfig
	wsCfg   config.WorkspaceConfig
	mgr     *Manager
	dog     *Watchdog
	wsMgr   *workspace.Manager
	httpSrv *http.Server
	ln      net.Listener
}

// NewServer creates a session server.
func NewServer(cfg config.SessionConfig, wsCfg config.WorkspaceConfig, st *store.Store) *Server {
	logDir := platform.SessionLogDir(cfg.LogDir)
	mgr := NewManager(ManagerConfig{
		OutputBufferLines: cfg.OutputBufferLines,
		LogDir:            logDir,
	})
	wsMgr := workspace.NewManager(st, &wsCfg)

	// Watchdog updates DB when sessions die.
	onUpdate := func(wsID, status string, pid int, sessionID, logPath string) {
		ctx := context.Background()
		wsMgr.UpdateSession(ctx, wsID, workspace.SessionUpdate{
			SessionID:     sessionID,
			SessionPID:    pid,
			SessionStatus: status,
			SessionLog:    logPath,
		})
	}

	dog := NewWatchdog(mgr, cfg.WatchdogInterval, onUpdate)

	return &Server{
		cfg:   cfg,
		wsCfg: wsCfg,
		mgr:   mgr,
		dog:   dog,
		wsMgr: wsMgr,
	}
}

// Run starts the HTTP server and watchdog. Blocks until ctx is canceled.
func (s *Server) Run(ctx context.Context) error {
	// Write PID file.
	pidPath := platform.SessionPIDFilePath()
	if err := os.WriteFile(pidPath, []byte(strconv.Itoa(os.Getpid())), 0o600); err != nil {
		return fmt.Errorf("write session PID: %w", err)
	}
	defer os.Remove(pidPath)

	// Clean up zombie sessions (MANAGED in DB but no process alive).
	s.cleanZombies(ctx)

	// Start watchdog.
	go s.dog.Run(ctx)

	// Set up HTTP routes.
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sessions", s.handleListSessions)
	mux.HandleFunc("GET /api/session/{id}/status", s.handleSessionStatus)
	mux.HandleFunc("POST /api/session/{id}/start", s.handleStartSession)
	mux.HandleFunc("POST /api/session/{id}/stop", s.handleStopSession)
	mux.HandleFunc("POST /api/session/{id}/send", s.handleSendInput)
	mux.HandleFunc("/ws/session/{id}", s.handleWS)
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		w.Write([]byte(`{"ok":true}`))
	})

	ln, err := net.Listen("tcp", s.cfg.Addr)
	if err != nil {
		return fmt.Errorf("session server listen: %w", err)
	}
	s.ln = ln
	s.httpSrv = &http.Server{
		Handler:      mux,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
	}

	ml.Info("session server started", "addr", ln.Addr().String())

	errCh := make(chan error, 1)
	go func() {
		if err := s.httpSrv.Serve(ln); err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
		close(errCh)
	}()

	select {
	case <-ctx.Done():
	case err := <-errCh:
		return err
	}

	// Graceful shutdown.
	ml.Info("session server shutting down")
	s.mgr.StopAll(ctx)

	shutCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.httpSrv.Shutdown(shutCtx)
}

// Addr returns the actual bound address.
func (s *Server) Addr() string {
	if s.ln != nil {
		return s.ln.Addr().String()
	}
	return s.cfg.Addr
}

func (s *Server) cleanZombies(ctx context.Context) {
	branches, err := s.wsMgr.List(ctx)
	if err != nil {
		return
	}
	for _, b := range branches {
		if b.SessionStatus == StatusManaged && b.SessionPID > 0 {
			if !platform.IsProcessAlive(b.SessionPID) {
				ml.Info("cleaning zombie session", "workspace", b.WorkspaceID, "stale_pid", b.SessionPID)
				s.wsMgr.UpdateSession(ctx, b.WorkspaceID, workspace.SessionUpdate{
					SessionStatus: StatusStop,
					SessionPID:    0,
				})
			}
		}
	}
}

// --- HTTP Handlers ---

func (s *Server) handleListSessions(w http.ResponseWriter, _ *http.Request) {
	sessions := s.mgr.List()
	type item struct {
		WorkspaceID string `json:"workspace_id"`
		SessionID   string `json:"session_id"`
		Status      string `json:"status"`
		PID         int    `json:"pid"`
		StartedAt   string `json:"started_at"`
		LogPath     string `json:"log_path"`
	}
	result := make([]item, 0, len(sessions))
	for _, sess := range sessions {
		result = append(result, item{
			WorkspaceID: sess.WorkspaceID,
			SessionID:   sess.SessionID,
			Status:      sess.Status,
			PID:         sess.PID,
			StartedAt:   sess.StartedAt.Format(time.RFC3339),
			LogPath:     sess.LogPath,
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"sessions": result})
}

func (s *Server) handleSessionStatus(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	info := s.mgr.Get(id)
	if info == nil {
		writeJSON(w, http.StatusOK, map[string]any{"workspace_id": id, "status": StatusStop})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"workspace_id": id,
		"status":       info.Status,
		"session_id":   info.SessionID,
		"pid":          info.PID,
		"started_at":   info.StartedAt.Format(time.RFC3339),
		"log_path":     info.LogPath,
	})
}

func (s *Server) handleStartSession(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")

	// Check if already active.
	if existing := s.mgr.Get(wsID); existing != nil {
		writeJSON(w, http.StatusConflict, map[string]any{"ok": false, "error": "session already active"})
		return
	}

	mode := r.URL.Query().Get("mode")
	if mode == "" {
		mode = "new"
	}

	// Look up the workspace directory.
	ctx := r.Context()
	branch, err := s.wsMgr.Get(ctx, wsID)
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": "workspace not found: " + wsID})
		return
	}

	// Build claude command line.
	cmdLine := "claude --dangerously-skip-permissions"
	var sessionID string
	if mode == "resume" && branch.SessionID != "" {
		sessionID = branch.SessionID
		cmdLine += " --resume " + branch.SessionID
	} else {
		sessionID = fmt.Sprintf("%s-%d", wsID, time.Now().Unix())
	}

	// Spawn ConPTY.
	term, err := NewConPTY(cmdLine, 120, 40)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": "spawn terminal: " + err.Error()})
		return
	}

	sess, err := s.mgr.Register(wsID, sessionID, term)
	if err != nil {
		term.Close()
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": err.Error()})
		return
	}

	// Update DB.
	s.wsMgr.UpdateSession(ctx, wsID, workspace.SessionUpdate{
		SessionID:     sessionID,
		SessionPID:    term.Pid(),
		SessionStatus: StatusManaged,
		SessionLog:    sess.LogPath,
	})

	writeJSON(w, http.StatusOK, map[string]any{
		"ok":         true,
		"session_id": sessionID,
		"pid":        term.Pid(),
	})
}

func (s *Server) handleStopSession(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	if err := s.mgr.Stop(r.Context(), wsID); err != nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": err.Error()})
		return
	}

	// Reset DB.
	s.wsMgr.UpdateSession(r.Context(), wsID, workspace.SessionUpdate{
		SessionStatus: StatusStop,
		SessionPID:    0,
	})

	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) handleSendInput(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	info := s.mgr.Get(wsID)
	if info == nil {
		writeJSON(w, http.StatusNotFound, map[string]any{"ok": false, "error": "no active session"})
		return
	}

	var body struct {
		Text string `json:"text"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]any{"ok": false, "error": "invalid body"})
		return
	}

	// Ensure newline for prompt submission.
	text := body.Text
	if !strings.HasSuffix(text, "\n") {
		text += "\n"
	}

	if err := info.SendInput([]byte(text)); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]any{"ok": false, "error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "text": text})
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	HandleWebSocket(s.mgr, w, r, wsID)
}

func writeJSON(w http.ResponseWriter, status int, data any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data) //nolint:errcheck
}
```

- [ ] **Step 2: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./internal/session/...
```

Expected: 성공

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/internal/session/server.go
git commit -m "feat(tools): session HTTP 서버 — REST API + WebSocket 엔드포인트"
```

---

## Task 8: Session CLI Commands

**Files:**
- Create: `internal/cli/session_cmd.go`
- Modify: `internal/cli/root.go`

- [ ] **Step 1: session_cmd.go 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/session"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func sessionCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "session",
		Short: "세션 서버 관리",
	}
	cmd.AddCommand(sessionRunCmd())
	cmd.AddCommand(sessionStartCmd())
	cmd.AddCommand(sessionStopCmd())
	cmd.AddCommand(sessionStatusCmd())
	cmd.AddCommand(sessionSendCmd())
	return cmd
}

func sessionRunCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "run",
		Short: "세션 서버 포그라운드 실행 (디버깅용)",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := platform.EnsureDataDir(); err != nil {
				return err
			}

			appCfg, err := config.Load(config.DefaultPath())
			if err != nil {
				return err
			}

			if !appCfg.Session.Enabled {
				return fmt.Errorf("session server disabled in config")
			}

			// Logging — same daily rotation as daemon.
			logDir := filepath.Join(platform.DataDir(), "logs")
			dailyWriter, dwErr := log.NewDailyWriter(log.DailyWriterConfig{
				Dir:     logDir,
				MaxDays: appCfg.Log.MaxDays,
			})
			if dwErr != nil {
				return fmt.Errorf("daily log writer: %w", dwErr)
			}
			defer dailyWriter.Close()
			logWriter := io.MultiWriter(os.Stderr, dailyWriter)
			log.Init(log.LogConfig{Level: appCfg.Log.Level, Writer: logWriter})

			// Open shared DB.
			dbPath := appCfg.Store.DBPath
			if dbPath == "" {
				dbPath = platform.DBPath()
			}
			st, err := store.Open(dbPath)
			if err != nil {
				return fmt.Errorf("open store: %w", err)
			}
			defer st.Close()

			srv := session.NewServer(appCfg.Session, appCfg.Workspace, st)

			ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
			defer cancel()
			return srv.Run(ctx)
		},
	}
}

func sessionStartCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "start",
		Short: "세션 서버 백그라운드 시작",
		RunE: func(cmd *cobra.Command, args []string) error {
			if isSessionRunning() {
				fmt.Println("session server already running")
				return nil
			}
			pid, err := startSessionProcess()
			if err != nil {
				return err
			}
			fmt.Printf("session server started (pid %d)\n", pid)
			return nil
		},
	}
}

func sessionStopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "세션 서버 종료",
		RunE: func(cmd *cobra.Command, args []string) error {
			pid, err := readSessionPID()
			if err != nil {
				return fmt.Errorf("session server not running")
			}
			proc, err := os.FindProcess(pid)
			if err != nil {
				return err
			}
			if err := proc.Signal(syscall.SIGTERM); err != nil {
				proc.Kill()
			}
			// Wait for PID file removal.
			for i := 0; i < 50; i++ {
				time.Sleep(100 * time.Millisecond)
				if !platform.IsProcessAlive(pid) {
					break
				}
			}
			fmt.Println("session server stopped")
			return nil
		},
	}
}

func sessionStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "세션 서버 상태",
		Run: func(cmd *cobra.Command, args []string) {
			if isSessionRunning() {
				pid, _ := readSessionPID()
				fmt.Printf("session server running (pid %d)\n", pid)
			} else {
				fmt.Println("session server not running")
			}
		},
	}
}

func sessionSendCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "send <workspace_id> <text>",
		Short: "세션에 텍스트 전송 (stdin 주입)",
		Args:  cobra.MinimumNArgs(2),
		RunE: func(cmd *cobra.Command, args []string) error {
			wsID := args[0]
			text := strings.Join(args[1:], " ")

			appCfg, _ := config.Load(config.DefaultPath())
			addr := appCfg.Session.Addr
			if addr == "" {
				addr = "localhost:7601"
			}

			// Use session server's REST API.
			url := fmt.Sprintf("http://%s/api/session/%s/send", addr, wsID)
			body := fmt.Sprintf(`{"text":%q}`, text)
			resp, err := sendHTTPPost(url, body)
			if err != nil {
				return fmt.Errorf("send failed: %w", err)
			}
			fmt.Println(resp)
			return nil
		},
	}
}

func isSessionRunning() bool {
	pid, err := readSessionPID()
	if err != nil {
		return false
	}
	return platform.IsProcessAlive(pid)
}

func readSessionPID() (int, error) {
	data, err := os.ReadFile(platform.SessionPIDFilePath())
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}

func startSessionProcess() (int, error) {
	exe, err := os.Executable()
	if err != nil {
		return 0, fmt.Errorf("resolve executable: %w", err)
	}
	child := exec.Command(exe, "session", "run")
	detachProcess(child)
	if err := child.Start(); err != nil {
		return 0, fmt.Errorf("start session server: %w", err)
	}

	// Wait for HTTP health check.
	appCfg, _ := config.Load(config.DefaultPath())
	addr := appCfg.Session.Addr
	if addr == "" {
		addr = "localhost:7601"
	}

	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		resp, err := http.Get("http://" + addr + "/health")
		if err == nil {
			resp.Body.Close()
			return child.Process.Pid, nil
		}
	}
	return 0, fmt.Errorf("session server failed to start within 5 seconds")
}

func sendHTTPPost(url, jsonBody string) (string, error) {
	resp, err := http.Post(url, "application/json", strings.NewReader(jsonBody))
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return string(body), nil
}
```

- [ ] **Step 2: root.go에 sessionCmd 등록**

`internal/cli/root.go` — `root.AddCommand(configCmd())` 다음에 추가:

```go
	root.AddCommand(sessionCmd())
```

- [ ] **Step 3: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./...
```

Expected: 성공

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/cli/session_cmd.go apex_tools/apex-agent/internal/cli/root.go
git commit -m "feat(tools): session CLI — run/start/stop/status/send 서브커맨드"
```

---

## Task 9: Reverse Proxy (Dashboard → Session Server)

**Files:**
- Create: `internal/httpd/proxy.go`
- Modify: `internal/httpd/server.go`
- Modify: `internal/httpd/routes.go`

- [ ] **Step 1: proxy.go — 리버스 프록시 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"io"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// SessionProxy forwards /api/session/* and /ws/session/* to the session server.
type SessionProxy struct {
	target  *url.URL
	httpRP  *httputil.ReverseProxy
	wsDialer *websocket.Dialer
}

// NewSessionProxy creates a reverse proxy to the session server.
func NewSessionProxy(targetAddr string) *SessionProxy {
	target, _ := url.Parse("http://" + targetAddr)
	httpRP := httputil.NewSingleHostReverseProxy(target)
	httpRP.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		ml.Debug("session proxy error", "path", r.URL.Path, "err", err)
		http.Error(w, `{"ok":false,"error":"session server unavailable"}`, http.StatusBadGateway)
	}

	return &SessionProxy{
		target: target,
		httpRP: httpRP,
		wsDialer: &websocket.Dialer{
			HandshakeTimeout: 10 * time.Second,
		},
	}
}

// HandleHTTP proxies regular HTTP requests to the session server.
func (p *SessionProxy) HandleHTTP(w http.ResponseWriter, r *http.Request) {
	p.httpRP.ServeHTTP(w, r)
}

// HandleWebSocket proxies a WebSocket connection to the session server.
func (p *SessionProxy) HandleWebSocket(w http.ResponseWriter, r *http.Request) {
	// Build backend URL.
	backendURL := "ws://" + p.target.Host + r.URL.Path
	if r.URL.RawQuery != "" {
		backendURL += "?" + r.URL.RawQuery
	}

	// Connect to backend.
	backendConn, resp, err := p.wsDialer.Dial(backendURL, nil)
	if err != nil {
		ml.Debug("ws proxy dial failed", "url", backendURL, "err", err)
		if resp != nil {
			http.Error(w, "session server unavailable", resp.StatusCode)
		} else {
			http.Error(w, "session server unavailable", http.StatusBadGateway)
		}
		return
	}
	defer backendConn.Close()

	// Upgrade client connection.
	upgrader := websocket.Upgrader{
		ReadBufferSize:  4096,
		WriteBufferSize: 4096,
		CheckOrigin:     func(r *http.Request) bool { return true },
	}
	clientConn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer clientConn.Close()

	// Bidirectional pipe.
	done := make(chan struct{})

	// Backend → Client
	go func() {
		defer close(done)
		for {
			mt, msg, err := backendConn.ReadMessage()
			if err != nil {
				return
			}
			if err := clientConn.WriteMessage(mt, msg); err != nil {
				return
			}
		}
	}()

	// Client → Backend
	for {
		mt, msg, err := clientConn.ReadMessage()
		if err != nil {
			break
		}
		if err := backendConn.WriteMessage(mt, msg); err != nil {
			break
		}
	}

	<-done
}

// IsWebSocket checks if a request is a WebSocket upgrade request.
func IsWebSocket(r *http.Request) bool {
	return strings.EqualFold(r.Header.Get("Upgrade"), "websocket")
}
```

- [ ] **Step 2: server.go에 SessionProxy 필드 추가**

`internal/httpd/server.go` — Server struct에 필드 추가:

```go
type Server struct {
	backlogMgr    BacklogQuerier
	handoffMgr    HandoffQuerier
	queueMgr      QueueQuerier
	router        dispatch.Dispatcher
	sessionProxy  *SessionProxy     // 추가
	httpSrv       *http.Server
	listener      net.Listener
	lastRequest   atomic.Int64
	pages         map[string]*template.Template
	addr          string
}
```

New 함수 시그니처 확장:

```go
func New(backlogMgr BacklogQuerier, handoffMgr HandoffQuerier, queueMgr QueueQuerier, router dispatch.Dispatcher, addr string, sessionAddr string) *Server {
```

생성자에서 프록시 초기화 (sessionAddr가 비어있지 않을 때):

```go
	if sessionAddr != "" {
		s.sessionProxy = NewSessionProxy(sessionAddr)
	}
```

- [ ] **Step 3: routes.go에 프록시 라우트 추가**

`internal/httpd/routes.go` — registerRoutes 함수 끝에 추가:

```go
	// Session proxy routes (→ :7601)
	if s.sessionProxy != nil {
		mux.HandleFunc("/api/session/", func(w http.ResponseWriter, r *http.Request) {
			s.sessionProxy.HandleHTTP(w, r)
		})
		mux.HandleFunc("/ws/session/", func(w http.ResponseWriter, r *http.Request) {
			if IsWebSocket(r) {
				s.sessionProxy.HandleWebSocket(w, r)
			} else {
				s.sessionProxy.HandleHTTP(w, r)
			}
		})
	}
```

- [ ] **Step 4: daemon_cmd.go — httpd.New 호출 업데이트**

`internal/cli/daemon_cmd.go` — httpd.New 호출 시 sessionAddr 추가:

```go
d.SetHTTPServerFactory(func(addr string) *httpd.Server {
	return httpd.New(bqa, hqa, qqa, d.Router(), addr, appCfg.Session.Addr)
})
```

nil-manager fallback도 업데이트:

```go
hs = httpd.New(nil, nil, nil, d.router, d.cfg.HTTP.Addr, "")
```

- [ ] **Step 5: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./...
```

Expected: 성공

- [ ] **Step 6: 커밋**

```bash
git add apex_tools/apex-agent/internal/httpd/proxy.go apex_tools/apex-agent/internal/httpd/server.go apex_tools/apex-agent/internal/httpd/routes.go apex_tools/apex-agent/internal/cli/daemon_cmd.go
git commit -m "feat(tools): 리버스 프록시 — :7600이 세션 서버(:7601) 통합"
```

---

## Task 10: Dashboard /branches Page & Templates

**Files:**
- Create: `internal/httpd/branches.go`
- Create: `internal/httpd/templates/branches.html`
- Create: `internal/httpd/templates/partials/branches.html`
- Create: `internal/httpd/templates/partials/blocked_badge.html`
- Create: `internal/httpd/static/terminal.js`
- Modify: `internal/httpd/server.go` — WorkspaceQuerier 추가
- Modify: `internal/httpd/render.go` — pageFiles에 "branches" 추가
- Modify: `internal/httpd/routes.go` — /branches 라우트 등록
- Modify: `internal/httpd/templates/layout.html` — nav에 Branches + ⚠ 뱃지 추가

- [ ] **Step 1: WorkspaceQuerier 인터페이스 추가**

`internal/httpd/server.go`에 인터페이스 추가:

```go
// WorkspaceQuerier abstracts workspace module queries for the dashboard.
type WorkspaceQuerier interface {
	DashboardBranchesList() ([]BranchInfo, error)
	DashboardBlockedCount() (int, error)
}
```

Server struct에 필드 추가:

```go
	workspaceMgr  WorkspaceQuerier  // 추가
```

New 함수 시그니처 확장:

```go
func New(backlogMgr BacklogQuerier, handoffMgr HandoffQuerier, queueMgr QueueQuerier, workspaceMgr WorkspaceQuerier, router dispatch.Dispatcher, addr string, sessionAddr string) *Server {
```

- [ ] **Step 2: BranchInfo 타입 추가**

`internal/httpd/queries.go`에 추가:

```go
// BranchInfo represents a workspace branch for the dashboard.
type BranchInfo struct {
	WorkspaceID    string
	Directory      string
	GitBranch      string
	GitStatus      string
	SessionStatus  string
	SessionID      string
	HandoffStatus  string // from LEFT JOIN active_branches
	BacklogIDs     string // comma-separated
	BlockedBacklogs []BlockedBacklogInfo
}

type BlockedBacklogInfo struct {
	ID            int
	Title         string
	BlockedReason string
}
```

- [ ] **Step 3: branches.go — 핸들러 구현**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import "net/http"

func (s *Server) handleBranches(w http.ResponseWriter, r *http.Request) {
	data := map[string]any{
		"Page": "branches",
	}
	if s.workspaceMgr != nil {
		branches, _ := s.workspaceMgr.DashboardBranchesList()
		data["Branches"] = branches
	}
	s.renderPage(w, "branches", data)
}

func (s *Server) handlePartialBranches(w http.ResponseWriter, r *http.Request) {
	if s.workspaceMgr == nil {
		s.renderHTMXError(w, "workspace not available")
		return
	}
	branches, err := s.workspaceMgr.DashboardBranchesList()
	if err != nil {
		s.renderHTMXError(w, err.Error())
		return
	}
	s.renderPartial(w, "branches-list", branches)
}

func (s *Server) handlePartialBlockedBadge(w http.ResponseWriter, r *http.Request) {
	count := 0
	if s.workspaceMgr != nil {
		count, _ = s.workspaceMgr.DashboardBlockedCount()
	}
	s.renderPartial(w, "blocked-badge", map[string]int{"Count": count})
}
```

- [ ] **Step 4: branches.html 템플릿 생성**

`internal/httpd/templates/branches.html`:

```html
{{define "content"}}
<h1>Branches</h1>
<div class="toolbar">
  <button hx-post="/api/workspace/scan" hx-swap="none" class="btn btn-sm">Scan</button>
  <button hx-post="/api/workspace/sync-all" hx-swap="none" class="btn btn-sm">Sync All</button>
</div>

<div id="branches-list"
     hx-get="/partials/branches"
     hx-trigger="load, every 2s"
     data-poll>
  {{template "branches-list" .Branches}}
</div>

<script src="https://cdn.jsdelivr.net/npm/@xterm/xterm@5/lib/xterm.min.js"></script>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@xterm/xterm@5/css/xterm.min.css">
<script src="https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0/lib/addon-fit.min.js"></script>
<script src="/static/terminal.js"></script>
{{end}}
```

- [ ] **Step 5: partials/branches.html 파셜 생성**

`internal/httpd/templates/partials/branches.html`:

```html
{{define "branches-list"}}
{{range .}}
<div class="branch-card" data-ws="{{.WorkspaceID}}">
  <div class="branch-header">
    <span class="branch-id">{{.WorkspaceID}}</span>
    <span class="branch-dir">{{.Directory}}</span>
  </div>
  <div class="branch-meta">
    <span class="git-branch">{{.GitBranch}}</span>
    <span class="git-status status-{{.GitStatus}}">{{.GitStatus}}</span>
    <span class="session-status status-{{.SessionStatus}}">{{.SessionStatus}}</span>
    {{if .HandoffStatus}}<span class="handoff-status">{{.HandoffStatus}}</span>{{end}}
  </div>
  {{range .BlockedBacklogs}}
  <div class="blocked-item">⚠ BACKLOG-{{.ID}}: {{.BlockedReason}}</div>
  {{end}}
  <div class="branch-actions">
    {{if eq .SessionStatus "STOP"}}
      <button onclick="startSession('{{.WorkspaceID}}', 'new')" class="btn btn-sm btn-primary">New Session</button>
      {{if .SessionID}}
      <button onclick="startSession('{{.WorkspaceID}}', 'resume')" class="btn btn-sm">Resume</button>
      {{end}}
    {{else if eq .SessionStatus "MANAGED"}}
      <button onclick="toggleTerminal('{{.WorkspaceID}}')" class="btn btn-sm btn-primary">Terminal</button>
      <button onclick="stopSession('{{.WorkspaceID}}')" class="btn btn-sm btn-danger">Stop</button>
    {{else}}
      <span class="text-muted">External session</span>
    {{end}}
    <button onclick="syncBranch('{{.WorkspaceID}}')" class="btn btn-sm">Sync</button>
  </div>
  <div class="terminal-container" id="term-{{.WorkspaceID}}" style="display:none;"></div>
</div>
{{else}}
<p class="text-muted">No branches found. Click "Scan" to discover workspaces.</p>
{{end}}
{{end}}
```

- [ ] **Step 6: partials/blocked_badge.html 생성**

`internal/httpd/templates/partials/blocked_badge.html`:

```html
{{define "blocked-badge"}}
{{if gt .Count 0}}
<a href="/backlog?filter=blocked" class="badge badge-warn" title="{{.Count}} blocked backlog(s)">⚠ {{.Count}}</a>
{{end}}
{{end}}
```

- [ ] **Step 7: terminal.js — xterm.js WebSocket 클라이언트**

```javascript
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

const terminals = {};

function toggleTerminal(wsId) {
  const el = document.getElementById('term-' + wsId);
  if (!el) return;
  if (el.style.display === 'none') {
    el.style.display = 'block';
    if (!terminals[wsId]) {
      createTerminal(wsId, el);
    }
  } else {
    el.style.display = 'none';
  }
}

function createTerminal(wsId, container) {
  const term = new Terminal({
    fontSize: 13,
    fontFamily: '"Cascadia Code", "Consolas", monospace',
    theme: { background: '#1a1b26', foreground: '#c0caf5' },
    cursorBlink: true,
    scrollback: 5000,
  });
  const fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(container);
  fit.fit();

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(proto + '//' + location.host + '/ws/session/' + wsId);
  ws.binaryType = 'arraybuffer';

  ws.onmessage = function(e) {
    term.write(new Uint8Array(e.data));
  };

  term.onData(function(data) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(new TextEncoder().encode(data));
    }
  });

  ws.onclose = function() {
    term.write('\r\n\x1b[31m[Session disconnected]\x1b[0m\r\n');
  };

  window.addEventListener('resize', function() { fit.fit(); });

  terminals[wsId] = { term: term, ws: ws, fit: fit };
}

function startSession(wsId, mode) {
  fetch('/api/session/' + wsId + '/start?mode=' + mode, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      if (data.ok) {
        // Auto-open terminal after short delay.
        setTimeout(function() { toggleTerminal(wsId); }, 500);
      } else {
        alert('Start failed: ' + (data.error || 'unknown'));
      }
    });
}

function stopSession(wsId) {
  fetch('/api/session/' + wsId + '/stop', { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      if (terminals[wsId]) {
        terminals[wsId].ws.close();
        terminals[wsId].term.dispose();
        delete terminals[wsId];
        var el = document.getElementById('term-' + wsId);
        if (el) { el.style.display = 'none'; el.innerHTML = ''; }
      }
    });
}

function syncBranch(wsId) {
  fetch('/api/workspace/' + wsId + '/sync', { method: 'POST' });
}
```

- [ ] **Step 8: layout.html — nav에 Branches + blocked badge 추가**

`internal/httpd/templates/layout.html`의 nav-links `<ul>` 안에 추가:

```html
    <li><a href="/branches" {{if eq .Page "branches"}}class="active"{{end}}>Branches</a></li>
```

nav-right div의 poll-rate 앞에 추가:

```html
    <span id="blocked-badge"
          hx-get="/partials/blocked-badge"
          hx-trigger="load, every 2s"
          data-poll></span>
```

- [ ] **Step 9: render.go — pageFiles에 branches 추가**

```go
var pageFiles = map[string]string{
	"dashboard": "templates/dashboard.html",
	"backlog":   "templates/backlog.html",
	"handoff":   "templates/handoff.html",
	"queue":     "templates/queue.html",
	"branches":  "templates/branches.html",
}
```

- [ ] **Step 10: routes.go에 /branches 라우트 추가**

```go
	// Branches page + partials
	mux.HandleFunc("GET /branches", s.handleBranches)
	mux.HandleFunc("GET /partials/branches", s.handlePartialBranches)
	mux.HandleFunc("GET /partials/blocked-badge", s.handlePartialBlockedBadge)
```

- [ ] **Step 11: daemon_cmd.go — WorkspaceQuerier 어댑터 + httpd.New 업데이트**

어댑터 struct 추가:

```go
type workspaceQuerierAdapter struct {
	mgr *workspacemod.Manager
}

func (a *workspaceQuerierAdapter) DashboardBranchesList() ([]httpd.BranchInfo, error) {
	ctx := context.Background()
	branches, err := a.mgr.List(ctx)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BranchInfo, len(branches))
	for i, b := range branches {
		result[i] = httpd.BranchInfo{
			WorkspaceID:   b.WorkspaceID,
			Directory:     b.Directory,
			GitBranch:     b.GitBranch,
			GitStatus:     b.GitStatus,
			SessionStatus: b.SessionStatus,
			SessionID:     b.SessionID,
		}
	}
	return result, nil
}

func (a *workspaceQuerierAdapter) DashboardBlockedCount() (int, error) {
	// Uses backlog manager's blocked count — cross-cut via store query.
	return 0, nil // Phase 3에서 backlog.Manager.DashboardBlockedCount() 연동
}
```

httpd.New 호출 업데이트:

```go
wqa := &workspaceQuerierAdapter{mgr: workspaceMod.Manager()}
d.SetHTTPServerFactory(func(addr string) *httpd.Server {
	return httpd.New(bqa, hqa, qqa, wqa, d.Router(), addr, appCfg.Session.Addr)
})
```

- [ ] **Step 12: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./...
```

Expected: 성공

- [ ] **Step 13: 커밋**

```bash
git add apex_tools/apex-agent/internal/httpd/ apex_tools/apex-agent/internal/cli/daemon_cmd.go
git commit -m "feat(tools): /branches 대시보드 — xterm.js 터미널 + blocked badge"
```

---

## Task 11: Workspace API Routes (Dashboard)

**Files:**
- Modify: `internal/httpd/routes.go`
- Modify: `internal/httpd/branches.go`

- [ ] **Step 1: workspace API 핸들러 추가**

`internal/httpd/branches.go`에 추가:

```go
func (s *Server) handleAPIWorkspace(w http.ResponseWriter, r *http.Request) {
	if s.workspaceMgr == nil {
		s.renderError(w, http.StatusServiceUnavailable, "workspace not available")
		return
	}
	branches, err := s.workspaceMgr.DashboardBranchesList()
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, branches)
}

func (s *Server) handleWorkspaceScan(w http.ResponseWriter, r *http.Request) {
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	result, err := s.router.Dispatch(r.Context(), "workspace", "scan", nil, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}

func (s *Server) handleWorkspaceSync(w http.ResponseWriter, r *http.Request) {
	wsID := r.PathValue("id")
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	params, _ := json.Marshal(map[string]string{"workspace_id": wsID})
	result, err := s.router.Dispatch(r.Context(), "workspace", "sync", params, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}

func (s *Server) handleWorkspaceSyncAll(w http.ResponseWriter, r *http.Request) {
	if s.router == nil {
		s.renderError(w, http.StatusServiceUnavailable, "router not available")
		return
	}
	params, _ := json.Marshal(map[string]any{"all": true})
	result, err := s.router.Dispatch(r.Context(), "workspace", "sync", params, "")
	if err != nil {
		s.renderError(w, http.StatusInternalServerError, err.Error())
		return
	}
	s.renderJSON(w, http.StatusOK, result)
}
```

- [ ] **Step 2: routes.go에 workspace API 라우트 등록**

```go
	// Workspace API
	mux.HandleFunc("GET /api/workspace", s.handleAPIWorkspace)
	mux.HandleFunc("POST /api/workspace/scan", s.handleWorkspaceScan)
	mux.HandleFunc("POST /api/workspace/{id}/sync", s.handleWorkspaceSync)
	mux.HandleFunc("POST /api/workspace/sync-all", s.handleWorkspaceSyncAll)
```

- [ ] **Step 3: branches.go에 json import 추가**

`import "encoding/json"` 추가.

- [ ] **Step 4: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./...
```

Expected: 성공

- [ ] **Step 5: 커밋**

```bash
git add apex_tools/apex-agent/internal/httpd/branches.go apex_tools/apex-agent/internal/httpd/routes.go
git commit -m "feat(tools): workspace REST API — scan/sync/list 대시보드 라우트"
```

---

## Task 12: Blocked Count Integration

**Files:**
- Modify: `internal/modules/workspace/manage.go`
- Modify: `internal/httpd/queries.go`
- Modify: `internal/cli/daemon_cmd.go`

- [ ] **Step 1: workspace Manager에 DashboardBlockedCount 추가**

`internal/modules/workspace/manage.go`에 추가:

```go
// DashboardBlockedCount returns the number of FIXING backlogs with a non-empty blocked_reason.
// Uses a direct cross-module query (backlog_items table) for dashboard efficiency.
func (m *Manager) DashboardBlockedCount(ctx context.Context) (int, error) {
	row := m.q.QueryRow(ctx, `
		SELECT COUNT(*) FROM backlog_items
		WHERE status = 'FIXING' AND blocked_reason IS NOT NULL AND blocked_reason != ''
	`)
	var count int
	err := row.Scan(&count)
	return count, err
}
```

- [ ] **Step 2: daemon_cmd.go — 어댑터 업데이트**

`workspaceQuerierAdapter.DashboardBlockedCount` 수정:

```go
func (a *workspaceQuerierAdapter) DashboardBlockedCount() (int, error) {
	return a.mgr.DashboardBlockedCount(context.Background())
}
```

- [ ] **Step 3: 빌드 확인**

```bash
cd apex_tools/apex-agent && go build ./...
```

Expected: 성공

- [ ] **Step 4: 커밋**

```bash
git add apex_tools/apex-agent/internal/modules/workspace/manage.go apex_tools/apex-agent/internal/cli/daemon_cmd.go
git commit -m "feat(tools): blocked_reason ⚠ 뱃지 — backlog 크로스 쿼리 연동"
```

---

## Task 13: E2E Tests

**Files:**
- Create: `e2e/session_server_test.go`
- Modify: `e2e/testenv/env.go` (필요 시)

- [ ] **Step 1: 세션 서버 E2E 테스트**

```go
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

func TestSessionServer_HealthCheck(t *testing.T) {
	env := testenv.New(t)
	_ = env // testenv starts daemon with all modules

	// Session server health check via daemon proxy.
	// Note: In E2E, session server is not started separately.
	// This test validates the proxy returns 502 when session server is down.
	resp, err := http.Get("http://" + env.HTTPAddr + "/api/session/test/status")
	if err != nil {
		t.Skipf("HTTP not available: %v", err)
	}
	defer resp.Body.Close()
	// Expect 502 (session server not running) or valid response.
	if resp.StatusCode != http.StatusBadGateway && resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		t.Errorf("unexpected status %d: %s", resp.StatusCode, body)
	}
}

func TestWorkspaceAPI_Scan(t *testing.T) {
	env := testenv.New(t)
	resp, err := env.Client.Send(env.Ctx, "workspace", "scan", nil, "")
	if err != nil {
		t.Fatalf("scan IPC: %v", err)
	}
	if !resp.OK {
		t.Fatalf("scan failed: %s", resp.Error)
	}
}

func TestWorkspaceAPI_List(t *testing.T) {
	env := testenv.New(t)
	resp, err := env.Client.Send(env.Ctx, "workspace", "list", nil, "")
	if err != nil {
		t.Fatalf("list IPC: %v", err)
	}
	if !resp.OK {
		t.Fatalf("list failed: %s", resp.Error)
	}
}

func TestDashboard_BranchesPage(t *testing.T) {
	env := testenv.New(t)
	if env.HTTPAddr == "" {
		t.Skip("HTTP not available")
	}

	resp, err := http.Get("http://" + env.HTTPAddr + "/branches")
	if err != nil {
		t.Fatalf("GET /branches: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("status = %d, want 200", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), "Branches") {
		t.Error("page does not contain 'Branches'")
	}
}

func TestDashboard_BlockedBadge(t *testing.T) {
	env := testenv.New(t)
	if env.HTTPAddr == "" {
		t.Skip("HTTP not available")
	}

	resp, err := http.Get("http://" + env.HTTPAddr + "/partials/blocked-badge")
	if err != nil {
		t.Fatalf("GET blocked-badge: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("status = %d, want 200", resp.StatusCode)
	}
}
```

- [ ] **Step 2: 테스트 실행**

```bash
cd apex_tools/apex-agent && go test ./e2e/... -run "TestSessionServer|TestWorkspaceAPI|TestDashboard_Branches|TestDashboard_Blocked" -v -count=1
```

Expected: PASS (session 프록시 테스트는 502 허용)

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/apex-agent/e2e/session_server_test.go
git commit -m "test(tools): session 서버 + 대시보드 E2E 테스트"
```

---

## Task 14: validate-build Hook 업데이트

**Files:**
- Confirm: `internal/modules/hook/validate_build.go` (또는 해당 hook 로직)

세션 서버 명령어(`apex-agent session`)가 validate-build hook에 의해 차단되지 않도록 확인. `apex-agent`와 `run-hook`은 이미 허용 목록에 있으므로 추가 변경 불필요할 가능성이 높지만 확인 필요.

- [ ] **Step 1: hook 허용 목록 확인**

```bash
cd apex_tools/apex-agent && grep -n "apex-agent\|session" internal/modules/hook/*.go | head -20
```

`apex-agent` 키워드가 허용 목록에 있으면 `session` 서브커맨드도 자동 통과. 변경 불필요.

- [ ] **Step 2: 필요 시 업데이트 + 커밋**

---

## Self-Review Checklist

1. **Spec coverage**: 설계서 §2(아키텍처) → Task 7, §3(DB) → 기존 workspace v1, §4(Config) → 기존, §5(workspace) → 기존, §6(세션) → Tasks 2-8, §7(대시보드) → Tasks 10-12, §8(FIX flow) → Task 10/11의 API, §9(REST API) → Tasks 7/9/11
2. **Placeholder scan**: 모든 task에 코드 블록 포함. TBD/TODO 없음.
3. **Type consistency**: `SessionInfo`, `StatusManaged/StatusStop`, `Manager`, `RingBuffer` — 전체 일관.
4. **Gap**: 설계서 §8 Backlog FIX flow (모달, 프롬프트 주입) → `/api/session/{id}/send`와 `terminal.js`의 `startSession()`으로 커버. 대시보드 메인 Summary 카드 확장은 기존 HTMX partial 패턴으로 경미한 추가이므로 별도 task 불필요.
