// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// mockIPC records calls and returns configurable results.
type mockIPC struct {
	calls  []string
	result map[string]any
	err    error
}

func (m *mockIPC) fn(action string, params map[string]any) (map[string]any, error) {
	m.calls = append(m.calls, action)
	return m.result, m.err
}

func setupPipelineTest(t *testing.T) (string, *backlog.Manager, func()) {
	t.Helper()
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "docs"), 0o755)

	dbPath := filepath.Join(dir, "test.db")
	s, err := store.Open(dbPath)
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	mig := store.NewMigrator(s)
	mod := backlog.New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		s.Close()
		t.Fatalf("migrate: %v", err)
	}
	return dir, mod.Manager(), func() { s.Close() }
}

// ── DropPipeline ──

func TestDropPipeline_OK(t *testing.T) {
	dir := initTestRepo(t)
	runGit(t, dir, "checkout", "-b", "feature/drop-test")
	ctx := context.Background()

	mock := &mockIPC{result: map[string]any{}}
	params := map[string]any{"branch": "test", "reason": "테스트"}

	err := DropPipeline(ctx, params, dir, mock.fn)
	if err != nil {
		t.Fatalf("DropPipeline: %v", err)
	}
	if len(mock.calls) != 1 || mock.calls[0] != "notify-drop" {
		t.Errorf("expected IPC call 'notify-drop', got: %v", mock.calls)
	}
}

func TestDropPipeline_IPCFails(t *testing.T) {
	dir := initTestRepo(t)
	mock := &mockIPC{err: fmt.Errorf("FIXING 백로그 잔존")}
	params := map[string]any{"branch": "test", "reason": "테스트"}

	err := DropPipeline(context.Background(), params, dir, mock.fn)
	if err == nil {
		t.Fatal("expected error when IPC fails")
	}
}

// ── StartPipeline ──

func TestStartPipeline_OK(t *testing.T) {
	dir := initTestRepo(t)

	_, mgr, cleanup := setupPipelineTest(t)
	defer cleanup()

	mock := &mockIPC{result: map[string]any{"notification_id": float64(1)}}
	params := map[string]any{"branch": "test", "summary": "start"}

	err := StartPipeline(context.Background(), "feature/start-test", params, dir, mgr, mock.fn)
	if err != nil {
		t.Fatalf("StartPipeline: %v", err)
	}
	// 브랜치가 생성되었는지 확인
	branch := runGit(t, dir, "branch", "--show-current")
	if branch != "feature/start-test" {
		t.Errorf("expected feature/start-test, got %s", branch)
	}
}

func TestStartPipeline_BranchExists(t *testing.T) {
	dir := initTestRepo(t)
	runGit(t, dir, "checkout", "-b", "feature/exists")
	runGit(t, dir, "checkout", "main")

	mock := &mockIPC{result: map[string]any{}}
	params := map[string]any{"branch": "test", "summary": "start"}

	err := StartPipeline(context.Background(), "feature/exists", params, dir, nil, mock.fn)
	if err == nil {
		t.Fatal("expected error for existing branch")
	}
	// IPC가 호출되지 않아야 함
	if len(mock.calls) != 0 {
		t.Errorf("IPC should not be called, got: %v", mock.calls)
	}
}

func TestStartPipeline_IPCFails(t *testing.T) {
	dir := initTestRepo(t)

	mock := &mockIPC{err: fmt.Errorf("daemon unavailable")}
	params := map[string]any{"branch": "test", "summary": "start"}

	err := StartPipeline(context.Background(), "feature/ipc-fail", params, dir, nil, mock.fn)
	if err == nil {
		t.Fatal("expected error when IPC fails")
	}
	// 브랜치가 생성되지 않아야 함 (IPC 실패 → Phase 3 미도달)
	branch := runGit(t, dir, "branch", "--show-current")
	if branch != "main" {
		t.Errorf("branch should remain main after IPC failure, got %s", branch)
	}
}

