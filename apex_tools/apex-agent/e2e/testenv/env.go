// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package testenv

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/handoff"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
)

type TestEnv struct {
	Dir        string
	ConfigPath string
	DBPath     string
	SocketAddr string
	Client     *ipc.Client
	Cancel     context.CancelFunc

	daemon  *daemon.Daemon
	done    chan error
	stopped bool // true after Stop() has drained done — prevents double-drain in cleanup
}

// Done returns the channel that receives the daemon's exit error.
// The caller should only peek at the channel (e.g., <-env.Done()), not drain it
// in a way that conflicts with Stop() or t.Cleanup. For tests that send an IPC
// shutdown and want to wait for the daemon to exit before manipulating files,
// call env.Stop() instead — it handles draining the channel correctly.
func (e *TestEnv) Done() <-chan error {
	return e.done
}

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
	}

	d, err := daemon.New(cfg)
	if err != nil {
		t.Fatalf("daemon.New: %v", err)
	}

	// Register all production modules
	d.Register(hook.New())
	d.Register(backlog.New(d.Store()))
	d.Register(handoff.New(d.Store()))
	d.Register(queue.New(d.Store()))

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()

	// Wait for readiness
	time.Sleep(200 * time.Millisecond)

	client := ipc.NewClient(socketAddr)

	env := &TestEnv{
		Dir:        dir,
		ConfigPath: filepath.Join(dir, "config.toml"),
		DBPath:     dbPath,
		SocketAddr: socketAddr,
		Client:     client,
		Cancel:     cancel,
		daemon:     d,
		done:       done,
	}

	t.Cleanup(func() {
		if !env.stopped {
			cancel()
			<-done
		}
	})

	return env
}

// Stop stops the daemon and marks the env as stopped so the t.Cleanup
// registered by New will not attempt a double-drain of the done channel.
func (e *TestEnv) Stop() {
	if e.stopped {
		return
	}
	e.stopped = true
	e.Cancel()
	<-e.done
}

// Restart starts a new daemon on the same environment.
func (e *TestEnv) Restart(t *testing.T) {
	t.Helper()

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
	d.Register(hook.New())
	d.Register(backlog.New(d.Store()))
	d.Register(handoff.New(d.Store()))
	d.Register(queue.New(d.Store()))

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- d.Run(ctx) }()
	time.Sleep(200 * time.Millisecond)

	e.Cancel = cancel
	e.daemon = d
	e.done = done
	e.stopped = false
	e.Client = ipc.NewClient(e.SocketAddr)

	t.Cleanup(func() {
		if !e.stopped {
			cancel()
			<-done
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
