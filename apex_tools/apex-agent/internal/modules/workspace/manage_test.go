// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func testStore(t *testing.T) *store.Store {
	t.Helper()
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func setupManager(t *testing.T) *Manager {
	t.Helper()
	s := testStore(t)
	mod := New(s, nil)
	mig := store.NewMigrator(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatal(err)
	}
	return mod.Manager()
}

func TestMigration_V1_CreatesLocalBranches(t *testing.T) {
	s := testStore(t)
	mod := New(s, nil)
	mig := store.NewMigrator(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatal(err)
	}

	ctx := context.Background()
	_, err := s.Exec(ctx, `INSERT INTO local_branches (workspace_id, directory) VALUES ('branch_02', '/tmp/ws/branch_02')`)
	if err != nil {
		t.Fatal("insert failed:", err)
	}

	var status string
	err = s.QueryRow(ctx, `SELECT session_status FROM local_branches WHERE workspace_id='branch_02'`).Scan(&status)
	if err != nil {
		t.Fatal("query failed:", err)
	}
	if status != "STOP" {
		t.Errorf("want default session_status=STOP, got %s", status)
	}

	// UNIQUE constraint on directory
	_, err = s.Exec(ctx, `INSERT INTO local_branches (workspace_id, directory) VALUES ('branch_03', '/tmp/ws/branch_02')`)
	if err == nil {
		t.Error("want UNIQUE error on directory, got nil")
	}
}

func TestUpsertAndGet(t *testing.T) {
	mgr := setupManager(t)
	ctx := context.Background()

	b := &LocalBranch{
		WorkspaceID: "branch_02",
		Directory:   "/tmp/ws/apex_pipeline_branch_02",
		GitBranch:   "feature/auth",
		GitStatus:   "CLEAN",
	}
	if err := mgr.Upsert(ctx, b); err != nil {
		t.Fatal(err)
	}

	got, err := mgr.Get(ctx, "branch_02")
	if err != nil {
		t.Fatal(err)
	}
	if got.Directory != b.Directory {
		t.Errorf("want dir=%s, got %s", b.Directory, got.Directory)
	}
	if got.SessionStatus != "STOP" {
		t.Errorf("want session_status=STOP, got %s", got.SessionStatus)
	}
}

func TestUpsert_UpdateExisting(t *testing.T) {
	mgr := setupManager(t)
	ctx := context.Background()

	b := &LocalBranch{WorkspaceID: "branch_02", Directory: "/tmp/ws/b02", GitBranch: "main", GitStatus: "CLEAN"}
	mgr.Upsert(ctx, b)

	b.GitBranch = "feature/new"
	b.GitStatus = "DIRTY"
	mgr.Upsert(ctx, b)

	got, _ := mgr.Get(ctx, "branch_02")
	if got.GitBranch != "feature/new" {
		t.Errorf("want git_branch=feature/new, got %s", got.GitBranch)
	}
	if got.GitStatus != "DIRTY" {
		t.Errorf("want git_status=DIRTY, got %s", got.GitStatus)
	}
}

func TestList(t *testing.T) {
	mgr := setupManager(t)
	ctx := context.Background()

	mgr.Upsert(ctx, &LocalBranch{WorkspaceID: "branch_01", Directory: "/tmp/ws/b01"})
	mgr.Upsert(ctx, &LocalBranch{WorkspaceID: "branch_02", Directory: "/tmp/ws/b02"})

	list, err := mgr.List(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 2 {
		t.Errorf("want 2 branches, got %d", len(list))
	}
}

func TestDelete(t *testing.T) {
	mgr := setupManager(t)
	ctx := context.Background()

	mgr.Upsert(ctx, &LocalBranch{WorkspaceID: "branch_02", Directory: "/tmp/ws/b02"})
	if err := mgr.Delete(ctx, "branch_02"); err != nil {
		t.Fatal(err)
	}
	_, err := mgr.Get(ctx, "branch_02")
	if err == nil {
		t.Error("want error after delete, got nil")
	}
}

func TestUpdateSession(t *testing.T) {
	mgr := setupManager(t)
	ctx := context.Background()

	mgr.Upsert(ctx, &LocalBranch{WorkspaceID: "branch_02", Directory: "/tmp/ws/b02"})

	err := mgr.UpdateSession(ctx, "branch_02", SessionUpdate{
		SessionID:     "ses-abc123",
		SessionPID:    12345,
		SessionStatus: "MANAGED",
		SessionLog:    "/tmp/logs/b02.log",
	})
	if err != nil {
		t.Fatal(err)
	}
	got, _ := mgr.Get(ctx, "branch_02")
	if got.SessionStatus != "MANAGED" {
		t.Errorf("want MANAGED, got %s", got.SessionStatus)
	}
	if got.SessionPID != 12345 {
		t.Errorf("want pid=12345, got %d", got.SessionPID)
	}
	if got.SessionID != "ses-abc123" {
		t.Errorf("want session_id=ses-abc123, got %s", got.SessionID)
	}
}

// --- Scanner tests ---

func createFakeWorkspace(t *testing.T, root, name string) string {
	t.Helper()
	dir := filepath.Join(root, name)
	os.MkdirAll(dir, 0o755)
	cmd := exec.Command("git", "init")
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git init: %s: %v", out, err)
	}
	cmd = exec.Command("git", "commit", "--allow-empty", "-m", "init")
	cmd.Dir = dir
	cmd.Env = append(os.Environ(), "GIT_AUTHOR_NAME=test", "GIT_AUTHOR_EMAIL=t@t", "GIT_COMMITTER_NAME=test", "GIT_COMMITTER_EMAIL=t@t")
	cmd.CombinedOutput()
	return dir
}

func TestScanDirectories(t *testing.T) {
	root := t.TempDir()
	createFakeWorkspace(t, root, "apex_pipeline_branch_01")
	createFakeWorkspace(t, root, "apex_pipeline_branch_02")
	os.MkdirAll(filepath.Join(root, "other_project"), 0o755)

	entries, err := ScanDirectories(root, "apex_pipeline")
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 {
		t.Errorf("want 2 entries, got %d", len(entries))
	}
}

func TestScanDirectories_GitStatus(t *testing.T) {
	root := t.TempDir()
	dir := createFakeWorkspace(t, root, "apex_pipeline_test")

	entries, _ := ScanDirectories(root, "apex_pipeline")
	if len(entries) == 0 {
		t.Fatal("want at least 1 entry")
	}
	if entries[0].GitStatus != "CLEAN" {
		t.Errorf("want CLEAN, got %s", entries[0].GitStatus)
	}

	os.WriteFile(filepath.Join(dir, "dirty.txt"), []byte("x"), 0o644)
	entries, _ = ScanDirectories(root, "apex_pipeline")
	if entries[0].GitStatus != "DIRTY" {
		t.Errorf("want DIRTY, got %s", entries[0].GitStatus)
	}
}

func TestManagerScan(t *testing.T) {
	s := testStore(t)
	root := t.TempDir()
	createFakeWorkspace(t, root, "apex_pipeline_branch_01")
	createFakeWorkspace(t, root, "apex_pipeline_branch_02")

	cfg := &config.WorkspaceConfig{Root: root, RepoName: "apex_pipeline", ScanOnStart: true}
	mod := New(s, cfg)
	mig := store.NewMigrator(s)
	mod.RegisterSchema(mig)
	mig.Migrate()

	ctx := context.Background()
	result, err := mod.Manager().Scan(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if result.Added != 2 {
		t.Errorf("want added=2, got %d", result.Added)
	}

	list, _ := mod.Manager().List(ctx)
	if len(list) != 2 {
		t.Errorf("want 2 branches, got %d", len(list))
	}
}

func TestManagerScan_RemovesMissing(t *testing.T) {
	s := testStore(t)
	root := t.TempDir()
	createFakeWorkspace(t, root, "apex_pipeline_branch_01")
	createFakeWorkspace(t, root, "apex_pipeline_branch_02")

	cfg := &config.WorkspaceConfig{Root: root, RepoName: "apex_pipeline"}
	mod := New(s, cfg)
	mig := store.NewMigrator(s)
	mod.RegisterSchema(mig)
	mig.Migrate()

	ctx := context.Background()
	mod.Manager().Scan(ctx)

	os.RemoveAll(filepath.Join(root, "apex_pipeline_branch_02"))
	result, _ := mod.Manager().Scan(ctx)
	if result.Removed != 1 {
		t.Errorf("want removed=1, got %d", result.Removed)
	}

	list, _ := mod.Manager().List(ctx)
	if len(list) != 1 {
		t.Errorf("want 1 branch after removal, got %d", len(list))
	}
}

func TestOnStart_ScansIfEnabled(t *testing.T) {
	s := testStore(t)
	root := t.TempDir()
	createFakeWorkspace(t, root, "apex_pipeline_test_ws")

	cfg := &config.WorkspaceConfig{Root: root, RepoName: "apex_pipeline", ScanOnStart: true}
	mod := New(s, cfg)
	mig := store.NewMigrator(s)
	mod.RegisterSchema(mig)
	mig.Migrate()

	ctx := context.Background()
	if err := mod.OnStart(ctx); err != nil {
		t.Fatal(err)
	}

	list, _ := mod.Manager().List(ctx)
	if len(list) != 1 {
		t.Errorf("want 1 branch after OnStart scan, got %d", len(list))
	}
}
