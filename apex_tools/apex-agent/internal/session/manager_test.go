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
		// Simulate EOF when no data — ensures readPump exits promptly.
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

	// readPump will exit quickly because mockTerminal returns EOF.
	// Wait for it before checking state.
	<-sess.done

	info := mgr.Get("test_ws")
	if info == nil {
		t.Fatal("Get returned nil while session active")
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
	t.Cleanup(func() { mgr.StopAll(context.Background()) })

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
	t.Cleanup(func() { mgr.StopAll(context.Background()) })

	mgr.Register("ws_dup", "id-1", newMockTerminal(1))
	_, err := mgr.Register("ws_dup", "id-2", newMockTerminal(2))
	if err == nil {
		t.Error("expected error on duplicate register")
	}
}
