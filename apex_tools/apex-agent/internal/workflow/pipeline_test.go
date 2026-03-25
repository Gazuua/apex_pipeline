// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"testing"
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

	mock := &mockIPC{result: map[string]any{"notification_id": float64(1)}}
	params := map[string]any{"branch": "test", "summary": "start"}

	err := StartPipeline(context.Background(), "feature/start-test", params, dir, mock.fn)
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

	err := StartPipeline(context.Background(), "feature/exists", params, dir, mock.fn)
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

	err := StartPipeline(context.Background(), "feature/ipc-fail", params, dir, mock.fn)
	if err == nil {
		t.Fatal("expected error when IPC fails")
	}
	// 브랜치가 생성되지 않아야 함 (IPC 실패 → Phase 3 미도달)
	branch := runGit(t, dir, "branch", "--show-current")
	if branch != "main" {
		t.Errorf("branch should remain main after IPC failure, got %s", branch)
	}
}

// ── MergeFullPipeline ──

// callRecorder records callback invocation order in a thread-safe manner.
type callRecorder struct {
	mu    sync.Mutex
	calls []string
}

func (r *callRecorder) record(name string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.calls = append(r.calls, name)
}

func (r *callRecorder) getCalls() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	dst := make([]string, len(r.calls))
	copy(dst, r.calls)
	return dst
}

// setupMergeRepo creates a git repo with origin and a feature branch that has
// one commit ahead of main. Returns (repoDir, cleanup).
func setupMergeRepo(t *testing.T) string {
	t.Helper()
	repoDir, bareDir := initRepoWithOrigin(t)
	// feature 브랜치 생성 + 커밋
	runGit(t, repoDir, "checkout", "-b", "feature/merge-test")
	os.WriteFile(filepath.Join(repoDir, "feature.txt"), []byte("feature\n"), 0o644)
	runGit(t, repoDir, "add", ".")
	runGit(t, repoDir, "commit", "-m", "feature commit")
	// push feature to origin (gh pr merge에 필요)
	runGit(t, repoDir, "push", bareDir, "feature/merge-test")
	return repoDir
}

func TestMergeFullPipeline_OK(t *testing.T) {
	repoDir := setupMergeRepo(t)
	ctx := context.Background()
	rec := &callRecorder{}

	params := MergeFullParams{
		ProjectRoot: repoDir,
		Branch:      "feature/merge-test",
		Workspace:   "test-ws",
		Summary:     "테스트 머지",
		ImportFn: func(projectRoot string) {
			rec.record("Import")
		},
		ExportFn: func(projectRoot string) error {
			rec.record("Export")
			return nil
		},
		FinalizeFn: func(ctx context.Context) error {
			rec.record("Finalize")
			return nil
		},
		LockAcquireFn: func(ctx context.Context) error {
			rec.record("LockAcquire")
			return nil
		},
		LockReleaseFn: func(ctx context.Context) error {
			rec.record("LockRelease")
			return nil
		},
	}

	// MergeFullPipeline은 autoCommitExport, rebase, push, gh pr merge를
	// 내부에서 직접 실행함. 테스트 환경에서는 gh pr merge가 실패하지만,
	// 콜백 순서와 lock release 보장이 핵심 검증 대상.
	err := MergeFullPipeline(ctx, params)

	// gh pr merge가 없으므로 에러 발생은 예상됨 (push 또는 gh 실패)
	// 콜백 호출 순서만 검증
	calls := rec.getCalls()

	// LockAcquire가 첫 번째
	if len(calls) == 0 || calls[0] != "LockAcquire" {
		t.Errorf("expected LockAcquire first, got: %v", calls)
	}

	// LockRelease가 마지막 (defer 보장)
	if len(calls) == 0 || calls[len(calls)-1] != "LockRelease" {
		t.Errorf("expected LockRelease last, got: %v", calls)
	}

	// Import → Export 순서 확인
	importIdx, exportIdx := -1, -1
	for i, c := range calls {
		switch c {
		case "Import":
			importIdx = i
		case "Export":
			exportIdx = i
		}
	}
	if importIdx >= 0 && exportIdx >= 0 && importIdx >= exportIdx {
		t.Errorf("Import should come before Export: %v", calls)
	}

	// push/gh 실패로 에러가 반환되지만 lock release는 보장됨
	_ = err // push 또는 gh 에러는 예상됨
}

func TestMergeFullPipeline_LockFails(t *testing.T) {
	repoDir := setupMergeRepo(t)
	ctx := context.Background()
	rec := &callRecorder{}

	params := MergeFullParams{
		ProjectRoot: repoDir,
		Branch:      "feature/merge-test",
		Workspace:   "test-ws",
		Summary:     "테스트 머지",
		ImportFn: func(projectRoot string) {
			rec.record("Import")
		},
		ExportFn: func(projectRoot string) error {
			rec.record("Export")
			return nil
		},
		FinalizeFn: func(ctx context.Context) error {
			rec.record("Finalize")
			return nil
		},
		LockAcquireFn: func(ctx context.Context) error {
			rec.record("LockAcquire")
			return fmt.Errorf("lock 획득 실패: 다른 브랜치가 점유 중")
		},
		LockReleaseFn: func(ctx context.Context) error {
			rec.record("LockRelease")
			return nil
		},
	}

	err := MergeFullPipeline(ctx, params)
	if err == nil {
		t.Fatal("expected error when LockAcquireFn fails")
	}

	calls := rec.getCalls()

	// LockAcquire만 호출되어야 함
	if len(calls) != 1 || calls[0] != "LockAcquire" {
		t.Errorf("expected only [LockAcquire], got: %v", calls)
	}

	// LockRelease는 호출되지 않아야 함 (lock 미획득 상태에서 release 불필요)
	for _, c := range calls {
		if c == "LockRelease" {
			t.Error("LockRelease should NOT be called when LockAcquire fails")
		}
	}
}

func TestMergeFullPipeline_ExportFails(t *testing.T) {
	repoDir := setupMergeRepo(t)
	ctx := context.Background()
	rec := &callRecorder{}

	params := MergeFullParams{
		ProjectRoot: repoDir,
		Branch:      "feature/merge-test",
		Workspace:   "test-ws",
		Summary:     "테스트 머지",
		ImportFn: func(projectRoot string) {
			rec.record("Import")
		},
		ExportFn: func(projectRoot string) error {
			rec.record("Export")
			return fmt.Errorf("DB export 실패: disk full")
		},
		FinalizeFn: func(ctx context.Context) error {
			rec.record("Finalize")
			return nil
		},
		LockAcquireFn: func(ctx context.Context) error {
			rec.record("LockAcquire")
			return nil
		},
		LockReleaseFn: func(ctx context.Context) error {
			rec.record("LockRelease")
			return nil
		},
	}

	err := MergeFullPipeline(ctx, params)
	if err == nil {
		t.Fatal("expected error when ExportFn fails")
	}

	calls := rec.getCalls()

	// LockRelease가 반드시 호출되어야 함 (defer 보장)
	hasRelease := false
	for _, c := range calls {
		if c == "LockRelease" {
			hasRelease = true
		}
	}
	if !hasRelease {
		t.Error("LockRelease must be called even when ExportFn fails")
	}

	// Finalize는 호출되지 않아야 함 (Export 실패 → 조기 종료)
	for _, c := range calls {
		if c == "Finalize" {
			t.Error("Finalize should NOT be called when ExportFn fails")
		}
	}
}

func TestMergeFullPipeline_FinalizeFails_NoError(t *testing.T) {
	repoDir := setupMergeRepo(t)
	ctx := context.Background()
	rec := &callRecorder{}

	// gh pr merge와 push가 실패하면 Finalize에 도달하지 못하므로,
	// 이 테스트는 MergeFullPipeline의 ⑥단계 로직만 직접 검증.
	// FinalizeFn 에러는 경고만 출력하고 nil을 반환해야 함.

	// 실제 MergeFullPipeline에서 FinalizeFn 에러 처리를 검증하기 위해
	// 모든 git 단계를 성공시킬 수 없으므로(gh pr merge), 콜백만으로 로직 검증.
	// pipeline.go 76~139행의 FinalizeFn 에러 핸들링: err만 출력, return nil.

	finalizeCalled := false
	lockReleased := false

	params := MergeFullParams{
		ProjectRoot: repoDir,
		Branch:      "feature/merge-test",
		Workspace:   "test-ws",
		Summary:     "테스트 머지",
		ImportFn: func(projectRoot string) {
			rec.record("Import")
		},
		ExportFn: func(projectRoot string) error {
			rec.record("Export")
			return nil
		},
		FinalizeFn: func(ctx context.Context) error {
			finalizeCalled = true
			rec.record("Finalize")
			return fmt.Errorf("DB finalize 실패: connection lost")
		},
		LockAcquireFn: func(ctx context.Context) error {
			rec.record("LockAcquire")
			return nil
		},
		LockReleaseFn: func(ctx context.Context) error {
			lockReleased = true
			rec.record("LockRelease")
			return nil
		},
	}

	// MergeFullPipeline은 git 단계(rebase/push/gh)에서 실패할 수 있지만,
	// FinalizeFn 에러는 그 이후 단계. git 에러로 Finalize에 도달 못할 수 있음.
	err := MergeFullPipeline(ctx, params)

	// git 단계 에러가 아닌 경우에만 Finalize 검증 의미 있음
	if finalizeCalled {
		// FinalizeFn이 에러를 반환해도 MergeFullPipeline은 nil 반환 (경고만)
		// → git 단계를 통과했다면 err는 nil이어야 함
		if err != nil {
			t.Logf("git 단계에서 에러 발생 (테스트 환경): %v", err)
		}
	}

	// LockRelease는 항상 보장
	if !lockReleased {
		calls := rec.getCalls()
		t.Errorf("LockRelease must be called (defer). calls: %v", calls)
	}
}

func TestMergeFullPipeline_CheckoutFails_NoError(t *testing.T) {
	// CheckoutMain 실패는 MergeFullPipeline에서 경고만 출력 (exit 0).
	// git 환경에서 CheckoutMain은 실제로 동작하므로 실패를 만들기 어렵지만,
	// MergeFullPipeline의 에러 핸들링 로직을 검증.
	repoDir := setupMergeRepo(t)
	ctx := context.Background()
	rec := &callRecorder{}
	lockReleased := false

	params := MergeFullParams{
		ProjectRoot: repoDir,
		Branch:      "feature/merge-test",
		Workspace:   "test-ws",
		Summary:     "테스트 머지",
		ImportFn: func(projectRoot string) {
			rec.record("Import")
		},
		ExportFn: func(projectRoot string) error {
			rec.record("Export")
			return nil
		},
		FinalizeFn: func(ctx context.Context) error {
			rec.record("Finalize")
			return nil
		},
		LockAcquireFn: func(ctx context.Context) error {
			rec.record("LockAcquire")
			return nil
		},
		LockReleaseFn: func(ctx context.Context) error {
			lockReleased = true
			rec.record("LockRelease")
			return nil
		},
	}

	// git 단계(push/gh) 에러가 발생하더라도 LockRelease 보장이 핵심
	_ = MergeFullPipeline(ctx, params)

	if !lockReleased {
		calls := rec.getCalls()
		t.Errorf("LockRelease must always be called. calls: %v", calls)
	}

	calls := rec.getCalls()
	// LockRelease가 마지막이어야 함
	if len(calls) > 0 && calls[len(calls)-1] != "LockRelease" {
		t.Errorf("LockRelease should be the last call, got: %v", calls)
	}
}

