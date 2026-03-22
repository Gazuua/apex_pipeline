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
		Timeframe: "NOW", Scope: "core", Type: "bug",
		Description: "Something is broken",
	})
	mgr.Add(&BacklogItem{
		ID: 2, Title: "Future task", Severity: "MAJOR",
		Timeframe: "IN_VIEW", Scope: "shared", Type: "design-debt",
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
		Timeframe: "NOW", Scope: "core", Type: "bug",
		Description: "Still open",
	})
	mgr.Add(&BacklogItem{
		ID: 2, Title: "Resolved issue", Severity: "MINOR",
		Timeframe: "NOW", Scope: "core", Type: "bug",
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
		Timeframe: "NOW", Scope: "core", Type: "bug",
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
		Timeframe: "DEFERRED", Scope: "docs", Type: "docs",
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
