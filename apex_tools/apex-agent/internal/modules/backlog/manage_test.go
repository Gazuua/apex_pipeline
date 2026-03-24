// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func setupTestDB(t *testing.T) *store.Store {
	t.Helper()
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
	t.Cleanup(func() { s.Close() })
	return s
}

// ── NextID ──

func TestNextID_EmptyDB(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)
	id, err := m.NextID(context.Background())
	if err != nil {
		t.Fatalf("NextID failed: %v", err)
	}
	if id != 1 {
		t.Errorf("expected NextID=1 on empty DB, got %d", id)
	}
}

func TestNextID_AfterInsert(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID:          3,
		Title:       "Test Item",
		Severity:    "MAJOR",
		Timeframe:   "NOW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "A test bug",
		Position:    1,
		Status:      "OPEN",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	id, err := m.NextID(context.Background())
	if err != nil {
		t.Fatalf("NextID failed: %v", err)
	}
	if id != 4 {
		t.Errorf("expected NextID=4 after inserting id=3, got %d", id)
	}
}

// ── Add ──

func TestAdd_Basic(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID:          1,
		Title:       "Fix memory leak",
		Severity:    "CRITICAL",
		Timeframe:   "NOW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "Memory leak in session handler",
		Related:     "BACKLOG-99",
		Position:    1,
		Status:      "OPEN",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got == nil {
		t.Fatal("expected item, got nil")
	}
	if got.Title != item.Title {
		t.Errorf("Title: want %q, got %q", item.Title, got.Title)
	}
	if got.Severity != item.Severity {
		t.Errorf("Severity: want %q, got %q", item.Severity, got.Severity)
	}
	if got.Timeframe != item.Timeframe {
		t.Errorf("Timeframe: want %q, got %q", item.Timeframe, got.Timeframe)
	}
	if got.Scope != item.Scope {
		t.Errorf("Scope: want %q, got %q", item.Scope, got.Scope)
	}
	if got.Type != item.Type {
		t.Errorf("Type: want %q, got %q", item.Type, got.Type)
	}
	if got.Description != item.Description {
		t.Errorf("Description: want %q, got %q", item.Description, got.Description)
	}
	if got.Related != item.Related {
		t.Errorf("Related: want %q, got %q", item.Related, got.Related)
	}
	if got.Status != "OPEN" {
		t.Errorf("Status: want %q, got %q", "OPEN", got.Status)
	}
	if got.CreatedAt == "" {
		t.Error("CreatedAt should not be empty")
	}
	if got.UpdatedAt == "" {
		t.Error("UpdatedAt should not be empty")
	}
}

func TestAdd_AutoPosition(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	// Add two items in the same timeframe without specifying position.
	item1 := &BacklogItem{
		ID:          1,
		Title:       "First",
		Severity:    "MAJOR",
		Timeframe:   "NOW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "First item",
	}
	item2 := &BacklogItem{
		ID:          2,
		Title:       "Second",
		Severity:    "MINOR",
		Timeframe:   "NOW",
		Scope:       "CORE",
		Type:        "TEST",
		Description: "Second item",
	}

	if err := m.Add(context.Background(),item1); err != nil {
		t.Fatalf("Add item1 failed: %v", err)
	}
	if err := m.Add(context.Background(),item2); err != nil {
		t.Fatalf("Add item2 failed: %v", err)
	}

	got1, _ := m.Get(context.Background(),1)
	got2, _ := m.Get(context.Background(),2)

	if got1 == nil || got2 == nil {
		t.Fatal("expected both items to exist")
	}
	if got1.Position != 1 {
		t.Errorf("first item position: want 1, got %d", got1.Position)
	}
	if got2.Position != 2 {
		t.Errorf("second item position: want 2, got %d", got2.Position)
	}
}

// ── Get ──

func TestGet_NotFound(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	got, err := m.Get(context.Background(),999)
	if err != nil {
		t.Fatalf("Get on non-existent id should not error, got: %v", err)
	}
	if got != nil {
		t.Errorf("expected nil for non-existent id, got %+v", got)
	}
}

func TestGet_Found(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID:          42,
		Title:       "Design debt",
		Severity:    "MINOR",
		Timeframe:   "DEFERRED",
		Scope:       "GATEWAY",
		Type:        "DESIGN_DEBT",
		Description: "Refactor routing table",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	got, err := m.Get(context.Background(),42)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got == nil {
		t.Fatal("expected item, got nil")
	}
	if got.ID != 42 {
		t.Errorf("ID: want 42, got %d", got.ID)
	}
	if got.Title != item.Title {
		t.Errorf("Title: want %q, got %q", item.Title, got.Title)
	}
}

// ── List ──

func TestList_ByTimeframe(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	items := []*BacklogItem{
		{ID: 1, Title: "NOW-1", Severity: "CRITICAL", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 2, Title: "NOW-2", Severity: "MAJOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 3, Title: "IN_VIEW-1", Severity: "MAJOR", Timeframe: "IN_VIEW", Scope: "CORE", Type: "TEST", Description: "d"},
	}
	for _, item := range items {
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	nowItems, err := m.List(context.Background(),ListFilter{Timeframe: "NOW", Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(nowItems) != 2 {
		t.Errorf("expected 2 NOW items, got %d", len(nowItems))
	}

	inViewItems, err := m.List(context.Background(),ListFilter{Timeframe: "IN_VIEW", Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(inViewItems) != 1 {
		t.Errorf("expected 1 IN_VIEW item, got %d", len(inViewItems))
	}
}

func TestList_ByStatus(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	items := []*BacklogItem{
		{ID: 1, Title: "Open Item", Severity: "MAJOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 2, Title: "Another Open", Severity: "MINOR", Timeframe: "IN_VIEW", Scope: "CORE", Type: "DOCS", Description: "d"},
	}
	for _, item := range items {
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}
	if err := m.Resolve(context.Background(),1, "FIXED"); err != nil {
		t.Fatalf("Resolve failed: %v", err)
	}

	openItems, err := m.List(context.Background(),ListFilter{Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(openItems) != 1 {
		t.Errorf("expected 1 open item, got %d", len(openItems))
	}

	resolvedItems, err := m.List(context.Background(),ListFilter{Status: "RESOLVED"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(resolvedItems) != 1 {
		t.Errorf("expected 1 resolved item, got %d", len(resolvedItems))
	}
}

func TestList_OrderByPosition(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	// Insert in reverse order — List should still return ordered by timeframe then position.
	items := []*BacklogItem{
		{ID: 10, Title: "B", Severity: "MINOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 9, Title: "A", Severity: "MAJOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 8, Title: "C", Severity: "CRITICAL", Timeframe: "IN_VIEW", Scope: "CORE", Type: "BUG", Description: "d"},
	}
	for _, item := range items {
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	all, err := m.List(context.Background(),ListFilter{Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(all) != 3 {
		t.Fatalf("expected 3 items, got %d", len(all))
	}

	// NOW items come before IN_VIEW; within timeframe ordered by position (insertion order).
	if all[0].Timeframe != "NOW" {
		t.Errorf("first item should be NOW, got %q", all[0].Timeframe)
	}
	if all[1].Timeframe != "NOW" {
		t.Errorf("second item should be NOW, got %q", all[1].Timeframe)
	}
	if all[2].Timeframe != "IN_VIEW" {
		t.Errorf("third item should be IN_VIEW, got %q", all[2].Timeframe)
	}
	// Positions within NOW should be ascending.
	if all[0].Position >= all[1].Position {
		t.Errorf("positions within NOW should ascend: pos[0]=%d pos[1]=%d", all[0].Position, all[1].Position)
	}
}

func TestList_NoFilter_ReturnsAllOpen(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	for i := 1; i <= 3; i++ {
		item := &BacklogItem{
			ID:          i,
			Title:       "Item",
			Severity:    "MAJOR",
			Timeframe:   "NOW",
			Scope:       "CORE",
			Type:        "BUG",
			Description: "d",
		}
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	// Empty filter with no Status set defaults to showing all open items.
	all, err := m.List(context.Background(),ListFilter{})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(all) != 3 {
		t.Errorf("expected 3 items with empty filter, got %d", len(all))
	}
}

// ── Resolve ──

func TestResolve_Basic(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID:          1,
		Title:       "Bug to fix",
		Severity:    "MAJOR",
		Timeframe:   "NOW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "Fix this",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}
	if err := m.Resolve(context.Background(),1, "FIXED"); err != nil {
		t.Fatalf("Resolve failed: %v", err)
	}

	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "RESOLVED" {
		t.Errorf("Status: want %q, got %q", "RESOLVED", got.Status)
	}
	if got.Resolution != "FIXED" {
		t.Errorf("Resolution: want %q, got %q", "FIXED", got.Resolution)
	}
	if got.ResolvedAt == "" {
		t.Error("ResolvedAt should not be empty after resolve")
	}
}

func TestResolve_NotFound(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	err := m.Resolve(context.Background(),999, "FIXED")
	if err == nil {
		t.Error("expected error when resolving non-existent item")
	}
}

// ── Check ──

func TestCheck_Exists(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID:          1,
		Title:       "Check test",
		Severity:    "MINOR",
		Timeframe:   "DEFERRED",
		Scope:       "INFRA",
		Type:        "INFRA",
		Description: "d",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	exists, status, err := m.Check(context.Background(),1)
	if err != nil {
		t.Fatalf("Check failed: %v", err)
	}
	if !exists {
		t.Error("expected exists=true")
	}
	if status != "OPEN" {
		t.Errorf("status: want %q, got %q", "OPEN", status)
	}
}

func TestCheck_NotExists(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	exists, status, err := m.Check(context.Background(),999)
	if err != nil {
		t.Fatalf("Check failed: %v", err)
	}
	if exists {
		t.Error("expected exists=false")
	}
	if status != "" {
		t.Errorf("status: want empty, got %q", status)
	}
}

// ── SetStatus ──

func TestSetStatus_Valid(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID: 1, Title: "Status test", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "test status transitions",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	// OPEN → FIXING
	if err := m.SetStatus(context.Background(),1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}
	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "FIXING" {
		t.Errorf("Status: want %q, got %q", "FIXING", got.Status)
	}

	// FIXING → RESOLVED
	if err := m.SetStatus(context.Background(),1, "RESOLVED"); err != nil {
		t.Fatalf("SetStatus RESOLVED failed: %v", err)
	}
	got, err = m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "RESOLVED" {
		t.Errorf("Status: want %q, got %q", "RESOLVED", got.Status)
	}
}

func TestSetStatus_NotFound(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	err := m.SetStatus(context.Background(),999, "FIXING")
	if err == nil {
		t.Fatal("expected error for non-existent item")
	}
}

func TestSetStatusWith_UsesTxStore(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID: 1, Title: "TxStore test", Severity: "MINOR",
		Timeframe: "IN_VIEW", Scope: "SHARED", Type: "TEST",
		Description: "test SetStatusWith via RunInTx",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	err := s.RunInTx(context.Background(), func(tx *store.TxStore) error {
		return m.SetStatusWith(context.Background(), tx, 1, "FIXING")
	})
	if err != nil {
		t.Fatalf("RunInTx + SetStatusWith failed: %v", err)
	}

	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "FIXING" {
		t.Errorf("Status: want %q, got %q", "FIXING", got.Status)
	}
}

// ── Release ──

func TestRelease_FixingToOpen(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID: 1, Title: "Release test", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "original description",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	// Set to FIXING first
	if err := m.SetStatus(context.Background(),1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}

	// Release should revert to OPEN
	if err := m.Release(context.Background(),1, "not enough time", "feature/test"); err != nil {
		t.Fatalf("Release failed: %v", err)
	}

	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "OPEN" {
		t.Errorf("Status after release: want %q, got %q", "OPEN", got.Status)
	}
}

func TestRelease_AppendsDescription(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	item := &BacklogItem{
		ID: 1, Title: "Release desc test", Severity: "MINOR",
		Timeframe: "DEFERRED", Scope: "TOOLS", Type: "DESIGN_DEBT",
		Description: "original",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	if err := m.SetStatus(context.Background(),1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}

	reason := "deferred to next sprint"
	branch := "feature/backlog-1"
	if err := m.Release(context.Background(),1, reason, branch); err != nil {
		t.Fatalf("Release failed: %v", err)
	}

	got, err := m.Get(context.Background(),1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}

	if !strings.Contains(got.Description, "[RELEASED]") {
		t.Errorf("Description should contain [RELEASED], got: %q", got.Description)
	}
	if !strings.Contains(got.Description, reason) {
		t.Errorf("Description should contain reason %q, got: %q", reason, got.Description)
	}
	if !strings.Contains(got.Description, branch) {
		t.Errorf("Description should contain branch %q, got: %q", branch, got.Description)
	}
	if !strings.HasPrefix(got.Description, "original") {
		t.Errorf("Description should start with original text, got: %q", got.Description)
	}
}

// ── Update ──

func TestUpdate_SingleField(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "원래 제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "원래 설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"title": "수정된 제목"})
	if err != nil {
		t.Fatalf("Update: %v", err)
	}

	items, _ := mgr.List(context.Background(),ListFilter{Status: "OPEN"})
	if len(items) != 1 || items[0].Title != "수정된 제목" {
		t.Errorf("expected '수정된 제목', got '%s'", items[0].Title)
	}
	if items[0].Severity != "MAJOR" {
		t.Errorf("severity should be preserved, got '%s'", items[0].Severity)
	}
}

func TestUpdate_MultipleFields(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{
		"severity":    "MINOR",
		"description": "수정된 설명",
	})
	if err != nil {
		t.Fatalf("Update: %v", err)
	}

	items, _ := mgr.List(context.Background(),ListFilter{Status: "OPEN"})
	if items[0].Severity != "MINOR" || items[0].Description != "수정된 설명" {
		t.Errorf("fields not updated: severity=%s desc=%s", items[0].Severity, items[0].Description)
	}
}

func TestUpdate_NotFound(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	err := mgr.Update(context.Background(),999, map[string]string{"title": "x"})
	if err == nil {
		t.Fatal("expected error for non-existent item")
	}
}

func TestUpdate_EmptyFields(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{})
	if err == nil {
		t.Fatal("expected error for empty fields map")
	}
}

func TestUpdate_UnknownField(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"nonexistent": "value"})
	if err == nil {
		t.Fatal("expected error for unknown field")
	}
}

func TestUpdate_ScopeField(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"scope": "CORE"})
	if err != nil {
		t.Fatalf("Update scope: %v", err)
	}

	got, _ := mgr.Get(context.Background(),1)
	if got.Scope != "CORE" {
		t.Errorf("expected scope=CORE, got %s", got.Scope)
	}
}

func TestUpdate_RelatedField(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"related": "42,99"})
	if err != nil {
		t.Fatalf("Update related: %v", err)
	}

	got, _ := mgr.Get(context.Background(),1)
	if got.Related != "42,99" {
		t.Errorf("expected related=42,99, got %s", got.Related)
	}
}

func TestUpdate_InvalidEnum(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"severity": "INVALID"})
	if err == nil {
		t.Fatal("expected error for invalid severity")
	}
}

func TestUpdate_InvalidScope(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{
		ID: 1, Title: "제목", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "TOOLS", Type: "BUG",
		Description: "설명",
	})

	err := mgr.Update(context.Background(),1, map[string]string{"scope": "INVALID_SCOPE"})
	if err == nil {
		t.Fatal("expected error for invalid scope")
	}
}

func TestUpdate_PositionReorder(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	// 3개 항목: position 1, 2, 3
	mgr.Add(context.Background(),&BacklogItem{ID: 1, Title: "A", Severity: "MAJOR", Timeframe: "NOW", Scope: "TOOLS", Type: "BUG", Description: "a"})
	mgr.Add(context.Background(),&BacklogItem{ID: 2, Title: "B", Severity: "MAJOR", Timeframe: "NOW", Scope: "TOOLS", Type: "BUG", Description: "b"})
	mgr.Add(context.Background(),&BacklogItem{ID: 3, Title: "C", Severity: "MAJOR", Timeframe: "NOW", Scope: "TOOLS", Type: "BUG", Description: "c"})

	// item 3을 position 1로 이동 → 기존 1,2가 2,3으로 밀림
	err := mgr.Update(context.Background(),3, map[string]string{"position": "1"})
	if err != nil {
		t.Fatalf("Update position: %v", err)
	}

	items, _ := mgr.List(context.Background(),ListFilter{Timeframe: "NOW", Status: "OPEN"})
	if len(items) != 3 {
		t.Fatalf("expected 3 items, got %d", len(items))
	}
	// position 순: C(1), A(2), B(3)
	if items[0].ID != 3 || items[1].ID != 1 || items[2].ID != 2 {
		t.Errorf("reorder failed: got IDs %d,%d,%d (expected 3,1,2)",
			items[0].ID, items[1].ID, items[2].ID)
	}
}

func TestUpdate_PositionInvalid(t *testing.T) {
	s := setupTestDB(t)
	defer s.Close()
	mgr := NewManager(s)

	mgr.Add(context.Background(),&BacklogItem{ID: 1, Title: "A", Severity: "MAJOR", Timeframe: "NOW", Scope: "TOOLS", Type: "BUG", Description: "a"})

	err := mgr.Update(context.Background(),1, map[string]string{"position": "0"})
	if err == nil {
		t.Fatal("expected error for position=0")
	}
	err = mgr.Update(context.Background(),1, map[string]string{"position": "abc"})
	if err == nil {
		t.Fatal("expected error for non-numeric position")
	}
}

// ── ListFixingForBranch ──

// ── Add — enum validation ──

func TestAdd_InvalidEnum(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	cases := []struct {
		name string
		item *BacklogItem
	}{
		{
			name: "lowercase severity",
			item: &BacklogItem{
				ID: 1, Title: "Bad severity", Severity: "major",
				Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d",
			},
		},
		{
			name: "lowercase timeframe",
			item: &BacklogItem{
				ID: 2, Title: "Bad timeframe", Severity: "MAJOR",
				Timeframe: "now", Scope: "CORE", Type: "BUG", Description: "d",
			},
		},
		{
			name: "lowercase type",
			item: &BacklogItem{
				ID: 3, Title: "Bad type", Severity: "MAJOR",
				Timeframe: "NOW", Scope: "CORE", Type: "bug", Description: "d",
			},
		},
		{
			name: "lowercase scope",
			item: &BacklogItem{
				ID: 4, Title: "Bad scope", Severity: "MAJOR",
				Timeframe: "NOW", Scope: "core", Type: "BUG", Description: "d",
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := m.Add(context.Background(),tc.item)
			if err == nil {
				t.Errorf("expected error for %s, got nil", tc.name)
			}
		})
	}
}

// ── ListFixingForBranch ──

func TestListFixingForBranch_Empty(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	// backlogIDs에 포함된 항목들이 FIXING이 아닌 경우 빈 리스트 반환
	item := &BacklogItem{
		ID: 1, Title: "Not fixing", Severity: "MAJOR",
		Timeframe: "NOW", Scope: "CORE", Type: "BUG",
		Description: "OPEN status item",
	}
	if err := m.Add(context.Background(),item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	fixing, err := m.ListFixingForBranch(context.Background(),"feature/test", []int{1})
	if err != nil {
		t.Fatalf("ListFixingForBranch: %v", err)
	}
	if len(fixing) != 0 {
		t.Errorf("expected empty list, got %v", fixing)
	}
}

func TestListFixingForBranch_Mixed(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	items := []*BacklogItem{
		{ID: 10, Title: "Open item", Severity: "MAJOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 20, Title: "Fixing item", Severity: "CRITICAL", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 30, Title: "Resolved item", Severity: "MINOR", Timeframe: "IN_VIEW", Scope: "SHARED", Type: "TEST", Description: "d"},
	}
	for _, item := range items {
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	// 20번만 FIXING 상태로 전이
	if err := m.SetStatus(context.Background(),20, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING: %v", err)
	}
	// 30번은 RESOLVED
	if err := m.Resolve(context.Background(),30, "FIXED"); err != nil {
		t.Fatalf("Resolve: %v", err)
	}

	fixing, err := m.ListFixingForBranch(context.Background(),"feature/test", []int{10, 20, 30})
	if err != nil {
		t.Fatalf("ListFixingForBranch: %v", err)
	}
	if len(fixing) != 1 {
		t.Fatalf("expected 1 FIXING item, got %d: %v", len(fixing), fixing)
	}
	if fixing[0] != 20 {
		t.Errorf("expected FIXING item ID 20, got %d", fixing[0])
	}
}

func TestListFixingForBranch_AllFixing(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	items := []*BacklogItem{
		{ID: 100, Title: "Fixing 1", Severity: "MAJOR", Timeframe: "NOW", Scope: "CORE", Type: "BUG", Description: "d"},
		{ID: 200, Title: "Fixing 2", Severity: "CRITICAL", Timeframe: "NOW", Scope: "SHARED", Type: "BUG", Description: "d"},
	}
	for _, item := range items {
		if err := m.Add(context.Background(),item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	// 둘 다 FIXING
	if err := m.SetStatus(context.Background(),100, "FIXING"); err != nil {
		t.Fatalf("SetStatus 100: %v", err)
	}
	if err := m.SetStatus(context.Background(),200, "FIXING"); err != nil {
		t.Fatalf("SetStatus 200: %v", err)
	}

	fixing, err := m.ListFixingForBranch(context.Background(),"feature/test", []int{100, 200})
	if err != nil {
		t.Fatalf("ListFixingForBranch: %v", err)
	}
	if len(fixing) != 2 {
		t.Fatalf("expected 2 FIXING items, got %d: %v", len(fixing), fixing)
	}

	// 반환된 ID들이 100, 200을 포함하는지 확인 (순서 무관)
	idSet := map[int]bool{}
	for _, id := range fixing {
		idSet[id] = true
	}
	if !idSet[100] || !idSet[200] {
		t.Errorf("expected IDs {100, 200}, got %v", fixing)
	}
}

func TestListFixingForBranch_NoBacklogs(t *testing.T) {
	s := setupTestDB(t)
	m := NewManager(s)

	// backlogIDs가 빈 슬라이스일 때 nil 반환 (early return)
	fixing, err := m.ListFixingForBranch(context.Background(),"feature/test", nil)
	if err != nil {
		t.Fatalf("ListFixingForBranch: %v", err)
	}
	if fixing != nil {
		t.Errorf("expected nil for empty backlogIDs, got %v", fixing)
	}

	// 빈 슬라이스도 동일하게 nil 반환
	fixing, err = m.ListFixingForBranch(context.Background(),"feature/test", []int{})
	if err != nil {
		t.Fatalf("ListFixingForBranch with empty slice: %v", err)
	}
	if fixing != nil {
		t.Errorf("expected nil for empty slice, got %v", fixing)
	}
}

// ── UpdateFromImport ──

// TestUpdateFromImport_PreservesUpdatedAt: import with identical fields does not change updated_at.
func TestUpdateFromImport_PreservesUpdatedAt(t *testing.T) {
	s := setupTestDB(t)
	mgr := NewManager(s)

	item := &BacklogItem{
		Title:       "test item",
		Severity:    "MAJOR",
		Timeframe:   "IN_VIEW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "desc",
	}
	if err := mgr.Add(context.Background(),item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	got, err := mgr.Get(context.Background(),item.ID)
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	originalUpdatedAt := got.UpdatedAt

	time.Sleep(1100 * time.Millisecond)

	// Import with identical fields — updated_at should NOT change.
	err = mgr.UpdateFromImport(context.Background(),item.ID, got.Title, got.Severity, got.Timeframe,
		got.Scope, got.Type, got.Description, got.Related, got.Position, got.Status)
	if err != nil {
		t.Fatalf("UpdateFromImport: %v", err)
	}

	got2, err := mgr.Get(context.Background(),item.ID)
	if err != nil {
		t.Fatalf("Get after import: %v", err)
	}
	if got2.UpdatedAt != originalUpdatedAt {
		t.Errorf("updated_at changed for identical import: %q → %q", originalUpdatedAt, got2.UpdatedAt)
	}
}

// TestUpdateFromImport_UpdatesOnChange: import with changed fields updates updated_at.
func TestUpdateFromImport_UpdatesOnChange(t *testing.T) {
	s := setupTestDB(t)
	mgr := NewManager(s)

	item := &BacklogItem{
		Title:       "test item",
		Severity:    "MAJOR",
		Timeframe:   "IN_VIEW",
		Scope:       "CORE",
		Type:        "BUG",
		Description: "desc",
	}
	if err := mgr.Add(context.Background(),item); err != nil {
		t.Fatalf("Add: %v", err)
	}

	got, err := mgr.Get(context.Background(),item.ID)
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	originalUpdatedAt := got.UpdatedAt

	time.Sleep(1100 * time.Millisecond)

	// Import with changed title — updated_at SHOULD change.
	err = mgr.UpdateFromImport(context.Background(),item.ID, "changed title", got.Severity, got.Timeframe,
		got.Scope, got.Type, got.Description, got.Related, got.Position, got.Status)
	if err != nil {
		t.Fatalf("UpdateFromImport: %v", err)
	}

	got2, err := mgr.Get(context.Background(),item.ID)
	if err != nil {
		t.Fatalf("Get after import: %v", err)
	}
	if got2.UpdatedAt == originalUpdatedAt {
		t.Error("expected updated_at to change for modified import")
	}
	if got2.Title != "changed title" {
		t.Errorf("expected title='changed title', got %q", got2.Title)
	}
}

// TestUpdateFromImport_NotFound: import for non-existent item returns an error.
func TestUpdateFromImport_NotFound(t *testing.T) {
	s := setupTestDB(t)
	mgr := NewManager(s)

	err := mgr.UpdateFromImport(context.Background(),999, "title", "MAJOR", "NOW", "CORE", "BUG", "desc", "", 1, "OPEN")
	if err == nil {
		t.Fatal("expected error for non-existent item, got nil")
	}
	if !strings.Contains(err.Error(), "not found") {
		t.Errorf("expected 'not found' in error message, got: %v", err)
	}
}
