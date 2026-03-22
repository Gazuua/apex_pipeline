// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func TestParseBacklogMD_Basic(t *testing.T) {
	content := `# BACKLOG

다음 발번: 5

---

## NOW

### #1. First issue
- **등급**: CRITICAL
- **스코프**: core
- **타입**: bug
- **설명**: Something broken

### #2. Second issue
- **등급**: MAJOR
- **스코프**: shared, tools
- **타입**: design-debt
- **연관**: #1
- **설명**: Needs refactoring

---

## IN VIEW

### #3. Future work
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **설명**: Write docs

---

## DEFERRED
`

	items, err := ParseBacklogMD(content)
	if err != nil {
		t.Fatal(err)
	}

	if len(items) != 3 {
		t.Fatalf("got %d items, want 3", len(items))
	}

	// Item 1
	if items[0].ID != 1 || items[0].Severity != "CRITICAL" || items[0].Timeframe != "NOW" {
		t.Errorf("item 1: %+v", items[0])
	}
	if items[0].Position != 1 {
		t.Errorf("item 1 position = %d, want 1", items[0].Position)
	}

	// Item 2
	if items[1].ID != 2 || items[1].Related != "1" || items[1].Timeframe != "NOW" {
		t.Errorf("item 2: %+v", items[1])
	}
	if items[1].Position != 2 {
		t.Errorf("item 2 position = %d, want 2", items[1].Position)
	}

	// Item 3
	if items[2].ID != 3 || items[2].Timeframe != "IN_VIEW" {
		t.Errorf("item 3: %+v", items[2])
	}
	if items[2].Position != 1 {
		t.Errorf("item 3 position = %d, want 1 (first in IN_VIEW)", items[2].Position)
	}
}

func TestParseBacklogHistoryMD_Basic(t *testing.T) {
	content := `# BACKLOG HISTORY

<!-- NEW_ENTRY_BELOW -->

### #128. AuthService locked_until fix
- **등급**: MAJOR | **스코프**: auth-svc | **타입**: bug
- **해결**: 2026-03-22 11:00:06 | **방식**: FIXED | **커밋**: 0c088c9
- **비고**: Time comparison added

### #39. CMakeLists hardcoded paths
- **등급**: MINOR | **스코프**: infra | **타입**: infra
- **해결**: 2026-03-22 11:00:06 | **방식**: FIXED
- **비고**: APEX_CORE_BIN_DIR variable
`

	items, err := ParseBacklogHistoryMD(content)
	if err != nil {
		t.Fatal(err)
	}

	if len(items) != 2 {
		t.Fatalf("got %d items, want 2", len(items))
	}

	if items[0].ID != 128 || items[0].Status != "resolved" || items[0].Resolution != "FIXED" {
		t.Errorf("item 128: %+v", items[0])
	}
	if items[0].Severity != "MAJOR" || items[0].Scope != "auth-svc" {
		t.Errorf("item 128 fields: %+v", items[0])
	}
}

func TestParseBacklogMD_RelatedFormatting(t *testing.T) {
	content := `## NOW

### #10. Multi related
- **등급**: MAJOR
- **스코프**: core
- **타입**: bug
- **연관**: #24, #29, #132
- **설명**: Has many related
`

	items, _ := ParseBacklogMD(content)
	if len(items) != 1 {
		t.Fatal("expected 1 item")
	}
	if items[0].Related != "24,29,132" {
		t.Errorf("related = %q, want '24,29,132'", items[0].Related)
	}
}

func TestImportItems_Basic(t *testing.T) {
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	mig := store.NewMigrator(s)
	mod := New(s)
	mod.RegisterSchema(mig)
	mig.Migrate()

	items := []BacklogItem{
		{ID: 1, Title: "Bug", Severity: "CRITICAL", Timeframe: "NOW", Scope: "core", Type: "bug", Description: "Fix it", Position: 1, Status: "open"},
		{ID: 2, Title: "Done", Severity: "MINOR", Timeframe: "NOW", Scope: "docs", Type: "docs", Description: "Was done", Position: 2, Status: "resolved", Resolution: "FIXED"},
	}

	count, err := mod.manager.ImportItems(items)
	if err != nil {
		t.Fatal(err)
	}
	if count != 2 {
		t.Errorf("imported %d, want 2", count)
	}

	// Verify open item
	item, _ := mod.manager.Get(1)
	if item.Status != "open" {
		t.Errorf("item 1 status = %q, want 'open'", item.Status)
	}

	// Verify resolved item
	item2, _ := mod.manager.Get(2)
	if item2.Status != "resolved" {
		t.Errorf("item 2 status = %q, want 'resolved'", item2.Status)
	}
}

func TestImportItems_SkipDuplicates(t *testing.T) {
	s, err := store.Open(":memory:")
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()

	mig := store.NewMigrator(s)
	mod := New(s)
	mod.RegisterSchema(mig)
	mig.Migrate()

	items := []BacklogItem{
		{ID: 1, Title: "Bug", Severity: "CRITICAL", Timeframe: "NOW", Scope: "core", Type: "bug", Description: "Fix", Position: 1, Status: "open"},
	}

	mod.manager.ImportItems(items)
	count, _ := mod.manager.ImportItems(items) // second import
	if count != 0 {
		t.Errorf("duplicate import count = %d, want 0", count)
	}
}
