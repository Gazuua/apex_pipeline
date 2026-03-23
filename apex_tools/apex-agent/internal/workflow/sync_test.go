// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"os"
	"path/filepath"
	"strings"
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

	n, err := SyncImport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport: %v", err)
	}
	if n != 2 {
		t.Errorf("expected 2 imported, got %d", n)
	}

	exists, status, _ := mgr.Check(1)
	if !exists {
		t.Fatal("item #1 not found in DB")
	}
	if status != "OPEN" {
		t.Errorf("expected OPEN, got %s", status)
	}
}

func TestSyncImport_NoFile(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	n, err := SyncImport(dir, mgr)
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
	SyncImport(dir, mgr)
	// 2차 import — 동일 결과여야 함
	n, err := SyncImport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncImport 2nd: %v", err)
	}
	// 이미 존재하는 항목도 UpdateFromImport로 count됨
	if n != 1 {
		t.Errorf("expected 1, got %d", n)
	}

	exists, status, _ := mgr.Check(1)
	if !exists || status != "OPEN" {
		t.Errorf("item #1: exists=%v status=%s", exists, status)
	}
}

// ── SyncExport ──

func TestSyncExport_WritesFile(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	item := &backlog.BacklogItem{
		ID: 1, Title: "테스트", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명", Status: "OPEN",
	}
	if err := mgr.Add(item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	_, err := SyncExport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	outPath := filepath.Join(dir, "docs", "BACKLOG.md")
	data, err := os.ReadFile(outPath)
	if err != nil {
		t.Fatalf("read exported file: %v", err)
	}
	content := string(data)
	if !strings.Contains(content, "테스트") {
		t.Error("exported file should contain item title")
	}
	if !strings.Contains(content, "## NOW") {
		t.Error("exported file should contain section headers")
	}
}

func TestSyncExport_WritesHistory(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	mgr.Add(&backlog.BacklogItem{
		ID: 1, Title: "해결됨", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	historyPath := filepath.Join(dir, "docs", "BACKLOG_HISTORY.md")
	os.WriteFile(historyPath, []byte("# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n"), 0o644)

	_, err := SyncExport(dir, mgr)
	if err != nil {
		t.Fatalf("SyncExport: %v", err)
	}

	data, _ := os.ReadFile(historyPath)
	if !strings.Contains(string(data), "해결됨") {
		t.Error("HISTORY should contain resolved item")
	}
}

func TestSyncExport_RoundTrip(t *testing.T) {
	dir, mgr, cleanup := setupSyncTest(t)
	defer cleanup()

	// Add item to DB
	item := &backlog.BacklogItem{
		ID: 42, Title: "라운드트립", Severity: "MINOR",
		Timeframe: "IN_VIEW", Scope: "CORE", Type: "PERF",
		Description: "라운드트립 테스트", Status: "OPEN",
	}
	mgr.Add(item)

	// Export → 파일 생성
	SyncExport(dir, mgr)

	// 새 DB로 import → 동일 항목 존재해야 함
	dir2, mgr2, cleanup2 := setupSyncTest(t)
	defer cleanup2()

	exported, _ := os.ReadFile(filepath.Join(dir, "docs", "BACKLOG.md"))
	os.WriteFile(filepath.Join(dir2, "docs", "BACKLOG.md"), exported, 0o644)

	n, err := SyncImport(dir2, mgr2)
	if err != nil {
		t.Fatalf("SyncImport round-trip: %v", err)
	}
	if n == 0 {
		t.Error("expected imported items in round-trip")
	}
	exists, _, _ := mgr2.Check(42)
	if !exists {
		t.Error("item #42 not found after round-trip")
	}
}
