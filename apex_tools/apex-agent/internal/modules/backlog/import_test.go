// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
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

	// Item 1 — scope/type normalized to UPPER_SNAKE_CASE
	if items[0].ID != 1 || items[0].Severity != "CRITICAL" || items[0].Timeframe != "NOW" {
		t.Errorf("item 1: %+v", items[0])
	}
	if items[0].Scope != "CORE" {
		t.Errorf("item 1 scope = %q, want CORE", items[0].Scope)
	}
	if items[0].Type != "BUG" {
		t.Errorf("item 1 type = %q, want BUG", items[0].Type)
	}
	if items[0].Position != 1 {
		t.Errorf("item 1 position = %d, want 1", items[0].Position)
	}

	// Item 2 — multi-scope normalized
	if items[1].ID != 2 || items[1].Related != "1" || items[1].Timeframe != "NOW" {
		t.Errorf("item 2: %+v", items[1])
	}
	if items[1].Scope != "SHARED, TOOLS" {
		t.Errorf("item 2 scope = %q, want 'SHARED, TOOLS'", items[1].Scope)
	}
	if items[1].Type != "DESIGN_DEBT" {
		t.Errorf("item 2 type = %q, want DESIGN_DEBT", items[1].Type)
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

	if items[0].ID != 128 || items[0].Status != StatusResolved || items[0].Resolution != "FIXED" {
		t.Errorf("item 128: %+v", items[0])
	}
	if items[0].Severity != "MAJOR" || items[0].Scope != "AUTH_SVC" {
		t.Errorf("item 128 fields: severity=%q scope=%q", items[0].Severity, items[0].Scope)
	}
	if items[0].Type != "BUG" {
		t.Errorf("item 128 type = %q, want BUG", items[0].Type)
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
		{ID: 1, Title: "Bug", Severity: "CRITICAL", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "Fix it", Position: 1, Status: "OPEN"},
		{ID: 2, Title: "Done", Severity: "MINOR", Timeframe: "NOW", Scope: "DOCS", Type: "DOCS", Description: "Was done", Position: 2, Status: StatusResolved, Resolution: "FIXED"},
	}

	count, err := mod.manager.ImportItems(context.Background(),items)
	if err != nil {
		t.Fatal(err)
	}
	if count != 2 {
		t.Errorf("imported %d, want 2", count)
	}

	// Verify open item
	item, _ := mod.manager.Get(context.Background(),1)
	if item.Status != "OPEN" {
		t.Errorf("item 1 status = %q, want 'OPEN'", item.Status)
	}

	// Verify resolved item
	item2, _ := mod.manager.Get(context.Background(),2)
	if item2.Status != "RESOLVED" {
		t.Errorf("item 2 status = %q, want 'RESOLVED'", item2.Status)
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
	if err := mig.Migrate(); err != nil {
		t.Fatalf("migrate: %v", err)
	}

	items := []BacklogItem{
		{ID: 1, Title: "Bug", Severity: "CRITICAL", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "Fix", Position: 1, Status: "OPEN"},
	}

	mod.manager.ImportItems(context.Background(),items)
	count, _ := mod.manager.ImportItems(context.Background(),items) // second import — updates existing
	if count != 1 {
		t.Errorf("duplicate import count = %d, want 1 (update)", count)
	}

	// Verify metadata is preserved after re-import.
	got, _ := mod.manager.Get(context.Background(),1)
	if got.Severity != "CRITICAL" || got.Timeframe != "NOW" {
		t.Errorf("re-imported item mismatch: severity=%s, timeframe=%s", got.Severity, got.Timeframe)
	}

	// Verify FIXING status is preserved on re-import.
	mod.manager.SetStatus(context.Background(),1, "FIXING")
	items[0].Timeframe = "IN_VIEW" // MD에서 timeframe 변경
	count, _ = mod.manager.ImportItems(context.Background(),items)
	if count != 1 {
		t.Errorf("update count = %d, want 1", count)
	}
	got, _ = mod.manager.Get(context.Background(),1)
	if got.Status != "FIXING" {
		t.Errorf("FIXING status should be preserved, got %s", got.Status)
	}
	if got.Timeframe != "IN_VIEW" {
		t.Errorf("timeframe should be updated to IN_VIEW, got %s", got.Timeframe)
	}

	// Verify RESOLVED status is also preserved — DB owns status, import never touches it.
	mod.manager.Resolve(context.Background(),1, "FIXED")
	items[0].Status = "OPEN" // MD says OPEN, but DB says RESOLVED
	count, _ = mod.manager.ImportItems(context.Background(),items)
	if count != 1 {
		t.Errorf("update count = %d, want 1", count)
	}
	got, _ = mod.manager.Get(context.Background(),1)
	if got.Status != "RESOLVED" {
		t.Errorf("RESOLVED status should be preserved, got %s", got.Status)
	}
}
