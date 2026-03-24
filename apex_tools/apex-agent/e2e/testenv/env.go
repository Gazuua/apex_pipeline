// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package testenv

import (
	"context"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/httpd"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/handoff"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

type TestEnv struct {
	Dir        string
	ConfigPath string
	DBPath     string
	SocketAddr string
	HTTPAddr   string // actual HTTP server address (e.g., "127.0.0.1:12345")
	Client     *ipc.Client
	Cancel     context.CancelFunc

	daemon  *daemon.Daemon
	done    chan error
	stopped bool
}

// Done returns the daemon completion channel.
func (e *TestEnv) Done() <-chan error { return e.done }

// New creates a fully isolated test environment with daemon running.
func New(t *testing.T) *TestEnv {
	t.Helper()

	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	socketAddr := testSocketAddr(t.Name())

	// Initialize logger (suppress noise in tests)
	log.Init(log.LogConfig{Level: "error", Writer: os.Stderr})

	cfg := daemon.Config{
		DBPath:      dbPath,
		PIDFilePath: filepath.Join(dir, "test.pid"),
		SocketAddr:  socketAddr,
		IdleTimeout: 5 * time.Minute,
		HTTP: config.HTTPConfig{
			Enabled: true,
			Addr:    "localhost:0", // random port
		},
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatalf("daemon.New: %v", err)
	}

	// Register all production modules
	backlogMod := backlog.New(d.Store())
	handoffMod := handoff.New(d.Store(), backlogMod.Manager())
	queueMod := queue.New(d.Store())
	d.Register(hook.New())
	d.Register(backlogMod)
	d.Register(handoffMod)
	d.Register(queueMod)

	// Junction cleaner (same as daemon_cmd.go)
	backlogMod.Manager().SetJunctionCleaner(func(q store.Querier, backlogID int) error {
		_, err := q.Exec(`DELETE FROM branch_backlogs WHERE backlog_id = ?`, backlogID)
		return err
	})

	// HTTP server factory with module manager adapters
	bqa := &testBacklogQuerier{mgr: backlogMod.Manager()}
	hqa := &testHandoffQuerier{mgr: handoffMod.Manager()}
	qqa := &testQueueQuerier{mgr: queueMod.Manager()}
	d.SetHTTPServerFactory(func(addr string) *httpd.Server {
		return httpd.New(bqa, hqa, qqa, d.Router(), addr)
	})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	// Wait for readiness — poll until socket is connectable (max 5s)
	waitForSocket(t, socketAddr)

	// Wait for HTTP server readiness
	httpAddr := waitForHTTP(t, d)

	client := ipc.NewClient(socketAddr)

	env := &TestEnv{
		Dir:        dir,
		ConfigPath: filepath.Join(dir, "config.toml"),
		DBPath:     dbPath,
		SocketAddr: socketAddr,
		HTTPAddr:   httpAddr,
		Client:     client,
		Cancel:     cancel,
		daemon:     d,
		done:       done,
	}

	t.Cleanup(func() {
		if !env.stopped {
			cancel()
			<-done
			env.stopped = true
		}
	})

	return env
}

// Stop stops the daemon and marks the env as stopped so cleanups skip.
func (e *TestEnv) Stop() {
	if e.stopped {
		return
	}
	e.stopped = true
	e.Cancel()
	<-e.done
}

// Restart starts a new daemon on the same environment.
// Caller must call Stop() before Restart().
func (e *TestEnv) Restart(t *testing.T) {
	t.Helper()

	// Unix 소켓 파일 잔여 제거 (Linux에서 flaky 방지)
	os.Remove(e.SocketAddr)

	cfg := daemon.Config{
		DBPath:      e.DBPath,
		PIDFilePath: filepath.Join(e.Dir, "test.pid"),
		SocketAddr:  e.SocketAddr,
		IdleTimeout: 5 * time.Minute,
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatalf("daemon.New on restart: %v", err)
	}
	backlogMod2 := backlog.New(d.Store())
	handoffMod2 := handoff.New(d.Store(), backlogMod2.Manager())
	queueMod2 := queue.New(d.Store())
	d.Register(hook.New())
	d.Register(backlogMod2)
	d.Register(handoffMod2)
	d.Register(queueMod2)

	backlogMod2.Manager().SetJunctionCleaner(func(q store.Querier, backlogID int) error {
		_, err := q.Exec(`DELETE FROM branch_backlogs WHERE backlog_id = ?`, backlogID)
		return err
	})

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()
	waitForSocket(t, e.SocketAddr)

	e.Cancel = cancel
	e.daemon = d
	e.done = done
	e.stopped = false
	e.Client = ipc.NewClient(e.SocketAddr)

	t.Cleanup(func() {
		if !e.stopped {
			cancel()
			<-done
			e.stopped = true
		}
	})
}

// InitGitRepo creates a temporary git repository for git-related tests.
func (e *TestEnv) InitGitRepo(t *testing.T) string {
	t.Helper()
	repoDir := filepath.Join(e.Dir, "repo")
	os.MkdirAll(repoDir, 0o755)

	run(t, repoDir, "git", "init")
	run(t, repoDir, "git", "config", "user.email", "test@test.com")
	run(t, repoDir, "git", "config", "user.name", "Test")
	os.WriteFile(filepath.Join(repoDir, "README.md"), []byte("# test\n"), 0o644)
	run(t, repoDir, "git", "add", ".")
	run(t, repoDir, "git", "commit", "-m", "initial commit")

	return repoDir
}

// waitForHTTP polls until the daemon's HTTP server is ready (max 5s).
func waitForHTTP(t *testing.T, d *daemon.Daemon) string {
	t.Helper()
	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		addr := d.HTTPAddr()
		if addr == "" {
			continue
		}
		resp, err := http.Get("http://" + addr + "/health")
		if err == nil {
			resp.Body.Close()
			return addr
		}
	}
	t.Fatalf("HTTP server not ready after 5s")
	return ""
}

// waitForSocket polls the IPC socket until it's connectable (max 5s).
func waitForSocket(t *testing.T, addr string) {
	t.Helper()
	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		conn, err := ipc.Dial(addr)
		if err == nil {
			conn.Close()
			return
		}
	}
	t.Fatalf("daemon socket not ready after 5s: %s", addr)
}

func testSocketAddr(name string) string {
	if runtime.GOOS == "windows" {
		return `\\.\pipe\apex-agent-e2e-` + sanitize(name)
	}
	return "/tmp/apex-agent-e2e-" + sanitize(name) + ".sock"
}

func sanitize(name string) string {
	result := make([]byte, 0, len(name))
	for _, b := range []byte(name) {
		if (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') {
			result = append(result, b)
		} else {
			result = append(result, '_')
		}
	}
	return string(result)
}

func run(t *testing.T, dir string, cmd string, args ...string) {
	t.Helper()
	c := exec.Command(cmd, args...)
	c.Dir = dir
	if out, err := c.CombinedOutput(); err != nil {
		t.Fatalf("%s %v failed: %v\n%s", cmd, args, err, out)
	}
}

// ── httpd interface adapters (mirrors cli/httpd_adapters.go) ─────────────────

type testBacklogQuerier struct{ mgr *backlog.Manager }

func (a *testBacklogQuerier) DashboardStatusCounts() (map[string]int, error) {
	return a.mgr.DashboardStatusCounts()
}
func (a *testBacklogQuerier) DashboardSeverityCounts() (map[string]int, error) {
	return a.mgr.DashboardSeverityCounts()
}
func (a *testBacklogQuerier) DashboardListItems(f httpd.BacklogFilter) ([]httpd.BacklogItem, error) {
	mf := backlog.DashboardFilter{
		Status: f.Status, Severity: f.Severity, Timeframe: f.Timeframe,
		Scope: f.Scope, Type: f.Type, SortBy: f.SortBy, SortDir: f.SortDir,
	}
	items, err := a.mgr.DashboardList(mf)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BacklogItem, len(items))
	for i, item := range items {
		result[i] = httpd.BacklogItem{
			ID: item.ID, Title: item.Title, Severity: item.Severity,
			Timeframe: item.Timeframe, Scope: item.Scope, Type: item.Type,
			Status: item.Status, Description: item.Description, Related: item.Related,
			Resolution: item.Resolution, CreatedAt: item.CreatedAt, UpdatedAt: item.UpdatedAt,
		}
	}
	return result, nil
}
func (a *testBacklogQuerier) DashboardGetItemByID(id int) (*httpd.BacklogItem, error) {
	item, err := a.mgr.DashboardGetByID(id)
	if err != nil {
		return nil, err
	}
	if item == nil {
		return nil, nil
	}
	return &httpd.BacklogItem{
		ID: item.ID, Title: item.Title, Severity: item.Severity,
		Timeframe: item.Timeframe, Scope: item.Scope, Type: item.Type,
		Status: item.Status, Description: item.Description, Related: item.Related,
		Resolution: item.Resolution, CreatedAt: item.CreatedAt, UpdatedAt: item.UpdatedAt,
	}, nil
}

type testHandoffQuerier struct{ mgr *handoff.Manager }

func (a *testHandoffQuerier) DashboardActiveBranchesList() ([]httpd.ActiveBranch, error) {
	branches, err := a.mgr.DashboardActiveBranches()
	if err != nil {
		return nil, err
	}
	result := make([]httpd.ActiveBranch, len(branches))
	for i, b := range branches {
		result[i] = httpd.ActiveBranch{
			Branch: b.Branch, WorkspaceID: b.WorkspaceID, GitBranch: b.GitBranch,
			Summary: b.Summary, Status: b.Status, BacklogIDs: b.BacklogIDs, CreatedAt: b.CreatedAt,
		}
	}
	return result, nil
}
func (a *testHandoffQuerier) DashboardActiveCount() (int, error) {
	return a.mgr.DashboardActiveCount()
}
func (a *testHandoffQuerier) DashboardBranchHistoryList(limit int) ([]httpd.BranchHistory, error) {
	history, err := a.mgr.DashboardBranchHistoryList(limit)
	if err != nil {
		return nil, err
	}
	result := make([]httpd.BranchHistory, len(history))
	for i, h := range history {
		result[i] = httpd.BranchHistory{
			ID: h.ID, Branch: h.Branch, GitBranch: h.GitBranch, Summary: h.Summary,
			Status: h.Status, BacklogIDs: h.BacklogIDs, StartedAt: h.StartedAt, FinishedAt: h.FinishedAt,
		}
	}
	return result, nil
}

type testQueueQuerier struct{ mgr *queue.Manager }

func (a *testQueueQuerier) DashboardQueueAll() ([]httpd.QueueEntry, error) {
	entries, err := a.mgr.DashboardQueueAll()
	if err != nil {
		return nil, err
	}
	result := make([]httpd.QueueEntry, len(entries))
	for i, e := range entries {
		result[i] = httpd.QueueEntry{
			Channel: e.Channel, Branch: e.Branch, Status: e.Status,
			CreatedAt: e.CreatedAt, FinishedAt: e.FinishedAt,
		}
		if e.DurationSec > 0 {
			h := e.DurationSec / 3600
			m := (e.DurationSec % 3600) / 60
			s := e.DurationSec % 60
			if h > 0 {
				result[i].Duration = fmt.Sprintf("%dh %dm %ds", h, m, s)
			} else if m > 0 {
				result[i].Duration = fmt.Sprintf("%dm %ds", m, s)
			} else {
				result[i].Duration = fmt.Sprintf("%ds", s)
			}
		}
	}
	return result, nil
}
func (a *testQueueQuerier) DashboardLockStatus(channel string) (bool, error) {
	return a.mgr.DashboardLockStatus(channel)
}
