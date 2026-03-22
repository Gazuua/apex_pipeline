// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"strings"
	"testing"

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
	id, err := m.NextID()
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	id, err := m.NextID()
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	got, err := m.Get(1)
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

	if err := m.Add(item1); err != nil {
		t.Fatalf("Add item1 failed: %v", err)
	}
	if err := m.Add(item2); err != nil {
		t.Fatalf("Add item2 failed: %v", err)
	}

	got1, _ := m.Get(1)
	got2, _ := m.Get(2)

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

	got, err := m.Get(999)
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	got, err := m.Get(42)
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
		if err := m.Add(item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	nowItems, err := m.List(ListFilter{Timeframe: "NOW", Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(nowItems) != 2 {
		t.Errorf("expected 2 NOW items, got %d", len(nowItems))
	}

	inViewItems, err := m.List(ListFilter{Timeframe: "IN_VIEW", Status: "OPEN"})
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
		if err := m.Add(item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}
	if err := m.Resolve(1, "FIXED"); err != nil {
		t.Fatalf("Resolve failed: %v", err)
	}

	openItems, err := m.List(ListFilter{Status: "OPEN"})
	if err != nil {
		t.Fatalf("List failed: %v", err)
	}
	if len(openItems) != 1 {
		t.Errorf("expected 1 open item, got %d", len(openItems))
	}

	resolvedItems, err := m.List(ListFilter{Status: "RESOLVED"})
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
		if err := m.Add(item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	all, err := m.List(ListFilter{Status: "OPEN"})
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
		if err := m.Add(item); err != nil {
			t.Fatalf("Add failed: %v", err)
		}
	}

	// Empty filter with no Status set defaults to showing all open items.
	all, err := m.List(ListFilter{})
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}
	if err := m.Resolve(1, "FIXED"); err != nil {
		t.Fatalf("Resolve failed: %v", err)
	}

	got, err := m.Get(1)
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

	err := m.Resolve(999, "FIXED")
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	exists, status, err := m.Check(1)
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

	exists, status, err := m.Check(999)
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	// OPEN → FIXING
	if err := m.SetStatus(1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}
	got, err := m.Get(1)
	if err != nil {
		t.Fatalf("Get failed: %v", err)
	}
	if got.Status != "FIXING" {
		t.Errorf("Status: want %q, got %q", "FIXING", got.Status)
	}

	// FIXING → RESOLVED
	if err := m.SetStatus(1, "RESOLVED"); err != nil {
		t.Fatalf("SetStatus RESOLVED failed: %v", err)
	}
	got, err = m.Get(1)
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

	err := m.SetStatus(999, "FIXING")
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	err := s.RunInTx(context.Background(), func(txs *store.Store) error {
		return m.SetStatusWith(txs, 1, "FIXING")
	})
	if err != nil {
		t.Fatalf("RunInTx + SetStatusWith failed: %v", err)
	}

	got, err := m.Get(1)
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	// Set to FIXING first
	if err := m.SetStatus(1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}

	// Release should revert to OPEN
	if err := m.Release(1, "not enough time", "feature/test"); err != nil {
		t.Fatalf("Release failed: %v", err)
	}

	got, err := m.Get(1)
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
	if err := m.Add(item); err != nil {
		t.Fatalf("Add failed: %v", err)
	}

	if err := m.SetStatus(1, "FIXING"); err != nil {
		t.Fatalf("SetStatus FIXING failed: %v", err)
	}

	reason := "deferred to next sprint"
	branch := "feature/backlog-1"
	if err := m.Release(1, reason, branch); err != nil {
		t.Fatalf("Release failed: %v", err)
	}

	got, err := m.Get(1)
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
