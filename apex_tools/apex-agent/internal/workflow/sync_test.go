// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupSyncTest(t *testing.T) (string, *backlog.Manager, func()) {
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

// ── SyncImport ──

func TestSyncImport_NewItems(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	// Legacy MD format — SyncImport should fall back to it
	md := `# BACKLOG

다음 발번: 3

---

## NOW

### #1. 테스트 이슈
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: 테스트 설명

### #2. 두번째 이슈
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: 두번째 설명

---

## IN VIEW

---

## DEFERRED
`
	os.WriteFile(filepath.Join(dir, "docs", "BACKLOG.md"), []byte(md), 0o644)

	n, err := SyncImport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport: %v", err)
	}
	if n != 2 {
		t.Errorf("expected 2 imported, got %d", n)
	}

	exists, status, _ := mgr.Check(ctx, 1)
	if !exists {
		t.Fatal("item #1 not found in DB")
	}
	if status != "OPEN" {
		t.Errorf("expected OPEN, got %s", status)
	}
}

func TestSyncImport_JSON(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	data := backlog.BacklogJSON{
		NextID: 3,
		Items: []backlog.BacklogItem{
			{ID: 1, Title: "JSON 이슈", Severity: "MAJOR", Timeframe: "NOW", Scope: "TOOLS", Type: "BUG", Description: "JSON 설명", Status: "OPEN", Position: 1},
			{ID: 2, Title: "두번째 JSON", Severity: "MINOR", Timeframe: "IN_VIEW", Scope: "CORE", Type: "PERF", Description: "설명2", Status: "OPEN", Position: 1},
		},
	}
	jsonData, _ := json.MarshalIndent(data, "", "  ")
	os.WriteFile(filepath.Join(dir, "docs", "BACKLOG.json"), jsonData, 0o644)

	n, err := SyncImport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport JSON: %v", err)
	}
	if n != 2 {
		t.Errorf("expected 2 imported, got %d", n)
	}
	exists, _, _ := mgr.Check(ctx, 1)
	if !exists {
		t.Fatal("item #1 not found in DB")
	}
}

func TestSyncImport_NoFile(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	n, err := SyncImport(context.Background(), dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport with no file: %v", err)
	}
	if n != 0 {
		t.Errorf("expected 0, got %d", n)
	}
}

func TestSyncImport_Idempotent(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	md := `# BACKLOG

다음 발번: 2

---

## NOW

### #1. 테스트 이슈
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: 테스트 설명

---

## IN VIEW

---

## DEFERRED
`
	os.WriteFile(filepath.Join(dir, "docs", "BACKLOG.md"), []byte(md), 0o644)

	// 1차 import
	SyncImport(ctx, dir, mgr)
	// 2차 import — 동일 결과여야 함
	n, err := SyncImport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport 2nd: %v", err)
	}
	// 이미 존재하는 항목도 UpdateFromImport로 count됨
	if n != 1 {
		t.Errorf("expected 1, got %d", n)
	}

	exists, status, _ := mgr.Check(ctx, 1)
	if !exists || status != "OPEN" {
		t.Errorf("item #1: exists=%v status=%s", exists, status)
	}
}

// ── SyncExport ──

func TestSyncExport_WritesJSON(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	item := &backlog.BacklogItem{
		ID: 1, Title: "테스트", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명", Status: "OPEN",
	}
	if err := mgr.Add(ctx, item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	_, err := SyncExport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	outPath := filepath.Join(dir, "docs", "BACKLOG.json")
	data, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatalf("read exported file: %v", err)
	}
	var exported backlog.BacklogJSON
	if err := json.Unmarshal(data, &exported); err != nil {
		t.Fatalf("unmarshal exported JSON: %v", err)
	}
	if len(exported.Items) != 1 {
		t.Errorf("expected 1 item, got %d", len(exported.Items))
	}
	if exported.Items[0].Title != "테스트" {
		t.Errorf("expected title '테스트', got %q", exported.Items[0].Title)
	}
}

func TestSyncExport_IncludesResolved(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	mgr.Add(ctx, &backlog.BacklogItem{
		ID: 1, Title: "해결됨", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(ctx, 1, "FIXED")

	_, err := SyncExport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	outPath := filepath.Join(dir, "docs", "BACKLOG.json")
	data, _ := os.ReadFile(outPath)
	var exported backlog.BacklogJSON
	json.Unmarshal(data, &exported)

	if len(exported.Items) != 1 {
		t.Fatalf("expected 1 item, got %d", len(exported.Items))
	}
	if exported.Items[0].Status != "RESOLVED" {
		t.Error("resolved item should be in JSON export")
	}
	if exported.Items[0].Resolution != "FIXED" {
		t.Error("resolution should be FIXED")
	}
}

func TestSyncExport_RoundTrip(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	// Add item to DB
	item := &backlog.BacklogItem{
		ID: 42, Title: "라운드트립", Severity: "MINOR",
		Timeframe: "IN_VIEW", Scope: "CORE", Type: "PERF",
		Description: "라운드트립 테스트", Status: "OPEN",
	}
	mgr.Add(ctx, item)

	// Export → JSON 파일 생성
	SyncExport(ctx, dir, mgr)

	// 새 DB로 import → 동일 항목 존재해야 함
	dir2, mgr2, cleanup2 := setupSyncTest(t)
	defer cleanup2()

	exported, _ := os.ReadFile(filepath.Join(dir, "docs", "BACKLOG.json"))
	os.WriteFile(filepath.Join(dir2, "docs", "BACKLOG.json"), exported, 0o644)

	n, err := SyncImport(ctx, dir2, mgr2)
	if err != nil {
		t.Fatalf("SyncImport round-trip: %v", err)
	}
	if n == 0 {
		t.Error("expected imported items in round-trip")
	}
	exists, _, _ := mgr2.Check(ctx, 42)
	if !exists {
		t.Error("item #42 not found after round-trip")
	}
}

func TestSyncExport_MigrateLegacyMD(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()
	ctx := context.Background()

	// Write legacy MD file
	md := `# BACKLOG

다음 발번: 2

---

## NOW

### #1. 레거시 이슈
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: 레거시 설명

---

## IN VIEW

---

## DEFERRED
`
	backlogMDPath := filepath.Join(dir, "docs", "BACKLOG.md")
	os.WriteFile(backlogMDPath, []byte(md), 0o644)

	// SyncImport from MD → DB
	SyncImport(ctx, dir, mgr)

	// SyncExport → should create BACKLOG.json and delete BACKLOG.md
	_, err := SyncExport(ctx, dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	// JSON should exist
	jsonPath := filepath.Join(dir, "docs", "BACKLOG.json")
	if _, err := os.Stat(jsonPath); os.IsNotExist(err) {
		t.Fatal("BACKLOG.json should exist after migration")
	}

	// Legacy MD should be deleted
	if _, err := os.Stat(backlogMDPath); !os.IsNotExist(err) {
		t.Error("BACKLOG.md should be deleted after migration")
	}
}
