// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupExportTestDB(t *testing.T) (*store.Store, *Manager) {
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	mig := store.NewMigrator(s)
	mod := New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		t.Fatal(err)
	}
	return s, mod.manager
}

func TestExport_EmptyDB(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	out, err := mgr.Export()
	if err != nil {
		t.Fatal(err)
	}

	// Must contain all three section headings
	if !strings.Contains(out, "## NOW") {
		t.Error("missing NOW section")
	}
	if !strings.Contains(out, "## IN VIEW") {
		t.Error("missing IN VIEW section")
	}
	if !strings.Contains(out, "## DEFERRED") {
		t.Error("missing DEFERRED section")
	}
	if !strings.Contains(out, "다음 발번: 1") {
		t.Error("missing/wrong 다음 발번")
	}
}

func TestExport_WithItems(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "Test issue", Severity: "CRITICAL",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "Something is broken",
	})
	mgr.Add(&BacklogItem{
		ID: 2, Title: "Future task", Severity: "MAJOR",
		Timeframe: "IN_VIEW", Scope: "SHARED", Type: "DESIGN_DEBT",
		Description: "Needs refactoring", Related: "1",
	})

	out, err := mgr.Export()
	if err != nil {
		t.Fatal(err)
	}

	// Check item 1 in NOW section
	if !strings.Contains(out, "### #1. Test issue") {
		t.Error("missing item #1")
	}
	if !strings.Contains(out, "- **등급**: CRITICAL") {
		t.Error("missing severity for #1")
	}

	// Check item 2 in IN VIEW section with related
	if !strings.Contains(out, "### #2. Future task") {
		t.Error("missing item #2")
	}
	if !strings.Contains(out, "- **연관**: #1") {
		t.Error("missing related for #2")
	}

	// 다음 발번 should be 3
	if !strings.Contains(out, "다음 발번: 3") {
		t.Error("다음 발번 should be 3")
	}
}

func TestExport_ResolvedExcluded(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "Open issue", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "Still open",
	})
	mgr.Add(&BacklogItem{
		ID: 2, Title: "Resolved issue", Severity: "MINOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "Already fixed",
	})
	mgr.Resolve(2, "FIXED")

	out, err := mgr.Export()
	if err != nil {
		t.Fatal(err)
	}

	if !strings.Contains(out, "### #1. Open issue") {
		t.Error("open item should be present")
	}
	if strings.Contains(out, "### #2. Resolved issue") {
		t.Error("resolved item should be excluded")
	}
}

func TestExport_RelatedFormatting(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 10, Title: "Multi-related", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "Links to many", Related: "24,29,132",
	})

	out, err := mgr.Export()
	if err != nil {
		t.Fatal(err)
	}

	if !strings.Contains(out, "- **연관**: #24, #29, #132") {
		t.Errorf("related formatting wrong, got: %s", out)
	}
}

func TestExport_NoRelatedLine(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "No relations", Severity: "MINOR",
		Timeframe: "DEFERRED", Scope: "DOCS", Type: "DOCS",
		Description: "Standalone item",
	})

	out, err := mgr.Export()
	if err != nil {
		t.Fatal(err)
	}

	// Should NOT have 연관 line
	lines := strings.Split(out, "\n")
	for _, l := range lines {
		if strings.Contains(l, "연관") {
			t.Errorf("should not have 연관 line for item without related, got: %s", l)
		}
	}
}

// ── ExportHistory ──

func TestExportHistory_NewResolved(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "해결된 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "테스트 설명",
	})
	mgr.Resolve(1, "FIXED")

	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n\n### #99. 기존 항목\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	if !strings.Contains(result, "해결된 이슈") {
		t.Error("should contain resolved item title")
	}
	if !strings.Contains(result, "#99. 기존 항목") {
		t.Error("should preserve existing history")
	}
	markerIdx := strings.Index(result, "<!-- NEW_ENTRY_BELOW -->")
	itemIdx := strings.Index(result, "해결된 이슈")
	if itemIdx < markerIdx {
		t.Error("resolved item should be after marker")
	}
}

func TestExportHistory_NoDuplicates(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "이미 있는 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n\n### #1. 이미 있는 이슈\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	count := strings.Count(result, "### #1.")
	if count != 1 {
		t.Errorf("expected 1 occurrence of #1, got %d", count)
	}
}

func TestExportHistory_NoMarker(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})
	mgr.Resolve(1, "FIXED")

	existingHistory := "# BACKLOG HISTORY\n\n### #99. 기존\n- old\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	if !strings.Contains(result, "<!-- NEW_ENTRY_BELOW -->") {
		t.Error("should restore marker")
	}
	if !strings.Contains(result, "이슈") {
		t.Error("should contain new item")
	}
}

func TestExportHistory_NoResolved(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	mgr.Add(&BacklogItem{
		ID: 1, Title: "열린 이슈", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	existingHistory := "# BACKLOG HISTORY\n\n<!-- NEW_ENTRY_BELOW -->\n"

	result, err := mgr.ExportHistory(existingHistory)
	if err != nil {
		t.Fatalf("ExportHistory: %v", err)
	}
	if result != existingHistory {
		t.Errorf("should not modify history when no resolved items\ngot: %q", result)
	}
}

// ── SafeExport ──

func TestSafeExport_ImportsThenExports(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	backlogMD := `# BACKLOG

다음 발번: 3

---

## NOW

### #1. Critical bug
- **등급**: CRITICAL
- **스코프**: CORE
- **타입**: BUG
- **설명**: Something is broken

### #2. Another issue
- **등급**: MAJOR
- **스코프**: SHARED
- **타입**: DESIGN_DEBT
- **설명**: Needs refactoring

---

## IN VIEW

---

## DEFERRED
`

	content, imported, err := mgr.SafeExport(backlogMD, "")
	if err != nil {
		t.Fatalf("SafeExport failed: %v", err)
	}

	if imported != 2 {
		t.Errorf("expected 2 imported items, got %d", imported)
	}

	// Verify exported content contains the imported items
	if !strings.Contains(content, "### #1. Critical bug") {
		t.Error("exported content should contain item #1")
	}
	if !strings.Contains(content, "### #2. Another issue") {
		t.Error("exported content should contain item #2")
	}
	if !strings.Contains(content, "## NOW") {
		t.Error("exported content should contain NOW section")
	}
}

func TestSafeExport_EmptyMDFiles(t *testing.T) {
	s, mgr := setupExportTestDB(t)
	defer s.Close()

	// Pre-populate DB with an item to verify export still works
	mgr.Add(&BacklogItem{
		ID: 1, Title: "Pre-existing", Severity: "MINOR",
		Timeframe: "DEFERRED", Scope: "DOCS", Type: "DOCS",
		Description: "Already in DB",
	})

	// Both MD files are empty strings — import phase is skipped
	content, imported, err := mgr.SafeExport("", "")
	if err != nil {
		t.Fatalf("SafeExport with empty MDs failed: %v", err)
	}

	if imported != 0 {
		t.Errorf("expected 0 imported with empty MDs, got %d", imported)
	}

	// DB item should still be exported
	if !strings.Contains(content, "### #1. Pre-existing") {
		t.Error("exported content should contain pre-existing DB item")
	}
}
