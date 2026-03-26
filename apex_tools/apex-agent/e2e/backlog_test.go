// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

// TestBacklog_CRUDAndExport verifies basic CRUD operations and export.
func TestBacklog_CRUDAndExport(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Step 1: next-id → id=1 (empty DB)
	resp, err := env.Client.Send(ctx, "backlog", "next-id", nil, "")
	if err != nil {
		t.Fatalf("next-id: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("next-id: expected OK, got error: %s", resp.Error)
	}
	var nextIDData map[string]any
	if err := json.Unmarshal(resp.Data, &nextIDData); err != nil {
		t.Fatalf("next-id: unmarshal: %v", err)
	}
	if nextIDData["id"].(float64) != 1 {
		t.Errorf("next-id: expected 1, got %v", nextIDData["id"])
	}

	// Step 2: add item
	resp, err = env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id":          1,
		"title":       "Test Bug",
		"severity":    "MAJOR",
		"timeframe":   "NOW",
		"scope":       "CORE",
		"type":        "BUG",
		"description": "A test bug for E2E validation",
	}, "")
	if err != nil {
		t.Fatalf("backlog.add: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.add: expected OK, got error: %s", resp.Error)
	}
	var addData map[string]any
	if err := json.Unmarshal(resp.Data, &addData); err != nil {
		t.Fatalf("backlog.add: unmarshal: %v", err)
	}
	if addData["id"].(float64) != 1 {
		t.Errorf("backlog.add: expected id=1, got %v", addData["id"])
	}

	// Step 3: list → verify 1 item
	resp, err = env.Client.Send(ctx, "backlog", "list", nil, "")
	if err != nil {
		t.Fatalf("backlog.list: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.list: expected OK, got error: %s", resp.Error)
	}
	var listData []any
	if err := json.Unmarshal(resp.Data, &listData); err != nil {
		t.Fatalf("backlog.list: unmarshal: %v", err)
	}
	if len(listData) != 1 {
		t.Errorf("backlog.list: expected 1 item, got %d", len(listData))
	}

	// Step 4: get by id → verify fields
	resp, err = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	if err != nil {
		t.Fatalf("backlog.get: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.get: expected OK, got error: %s", resp.Error)
	}
	var getItem map[string]any
	if err := json.Unmarshal(resp.Data, &getItem); err != nil {
		t.Fatalf("backlog.get: unmarshal: %v", err)
	}
	if getItem["title"] != "Test Bug" {
		t.Errorf("backlog.get: expected Title='Test Bug', got %v", getItem["title"])
	}
	if getItem["severity"] != "MAJOR" {
		t.Errorf("backlog.get: expected Severity='MAJOR', got %v", getItem["severity"])
	}
	if getItem["timeframe"] != "NOW" {
		t.Errorf("backlog.get: expected Timeframe='NOW', got %v", getItem["timeframe"])
	}
	if getItem["status"] != "OPEN" {
		t.Errorf("backlog.get: expected Status='OPEN', got %v", getItem["status"])
	}

	// Step 5: resolve → check status
	resp, err = env.Client.Send(ctx, "backlog", "resolve", map[string]any{
		"id":         1,
		"resolution": "FIXED",
	}, "")
	if err != nil {
		t.Fatalf("backlog.resolve: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.resolve: expected OK, got error: %s", resp.Error)
	}
	var resolveData map[string]string
	if err := json.Unmarshal(resp.Data, &resolveData); err != nil {
		t.Fatalf("backlog.resolve: unmarshal: %v", err)
	}
	if resolveData["status"] != "resolved" {
		t.Errorf("backlog.resolve: expected status='resolved', got %q", resolveData["status"])
	}

	// Add a new open item for export verification
	env.Client.Send(ctx, "backlog", "add", map[string]any{ //nolint:errcheck
		"id":          2,
		"title":       "Export Verification Item",
		"severity":    "MINOR",
		"timeframe":   "NOW",
		"scope":       "TOOLS",
		"type":        "PERF",
		"description": "Verify export output",
	}, "")

	// Step 6: export → verify content includes both open AND resolved items (JSON format)
	resp, err = env.Client.Send(ctx, "backlog", "export", nil, "")
	if err != nil {
		t.Fatalf("backlog.export: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.export: expected OK, got error: %s", resp.Error)
	}
	var exportData map[string]string
	if err := json.Unmarshal(resp.Data, &exportData); err != nil {
		t.Fatalf("backlog.export: unmarshal: %v", err)
	}
	content := exportData["content"]
	if content == "" {
		t.Fatal("backlog.export: content is empty")
	}
	if !strings.Contains(content, "Export Verification Item") {
		t.Error("backlog.export: content missing open item title")
	}
	// JSON export includes ALL items (open + resolved)
	if !strings.Contains(content, "Test Bug") {
		t.Error("backlog.export: content should include resolved item 'Test Bug'")
	}
}

// TestBacklog_MigrationRoundtrip verifies adding items with different timeframes,
// listing all, filtering by timeframe, and exporting.
func TestBacklog_MigrationRoundtrip(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Step 1: Add 3 items with different timeframes
	items := []map[string]any{
		{
			"id": 1, "title": "Now Item", "severity": "CRITICAL",
			"timeframe": "NOW", "scope": "CORE", "type": "BUG",
			"description": "Urgent fix needed",
		},
		{
			"id": 2, "title": "InView Item", "severity": "MAJOR",
			"timeframe": "IN_VIEW", "scope": "SHARED", "type": "DESIGN_DEBT",
			"description": "Refactor in view",
		},
		{
			"id": 3, "title": "Deferred Item", "severity": "MINOR",
			"timeframe": "DEFERRED", "scope": "TOOLS", "type": "PERF",
			"description": "Nice to have",
		},
	}

	for _, item := range items {
		resp, err := env.Client.Send(ctx, "backlog", "add", item, "")
		if err != nil {
			t.Fatalf("backlog.add %v: transport error: %v", item["title"], err)
		}
		if !resp.OK {
			t.Fatalf("backlog.add %v: expected OK, got error: %s", item["title"], resp.Error)
		}
	}

	// Step 2: List all → verify 3 items
	resp, err := env.Client.Send(ctx, "backlog", "list", nil, "")
	if err != nil {
		t.Fatalf("backlog.list all: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.list all: expected OK, got error: %s", resp.Error)
	}
	var allItems []any
	if err := json.Unmarshal(resp.Data, &allItems); err != nil {
		t.Fatalf("backlog.list all: unmarshal: %v", err)
	}
	if len(allItems) != 3 {
		t.Errorf("backlog.list all: expected 3 items, got %d", len(allItems))
	}

	// Step 3: List with timeframe filter → verify correct subset
	timeframes := []struct {
		filter   string
		expected int
		title    string
	}{
		{"NOW", 1, "Now Item"},
		{"IN_VIEW", 1, "InView Item"},
		{"DEFERRED", 1, "Deferred Item"},
	}

	for _, tc := range timeframes {
		resp, err := env.Client.Send(ctx, "backlog", "list", map[string]any{
			"timeframe": tc.filter,
		}, "")
		if err != nil {
			t.Fatalf("backlog.list timeframe=%s: transport error: %v", tc.filter, err)
		}
		if !resp.OK {
			t.Fatalf("backlog.list timeframe=%s: expected OK, got error: %s", tc.filter, resp.Error)
		}
		var filtered []any
		if err := json.Unmarshal(resp.Data, &filtered); err != nil {
			t.Fatalf("backlog.list timeframe=%s: unmarshal: %v", tc.filter, err)
		}
		if len(filtered) != tc.expected {
			t.Errorf("backlog.list timeframe=%s: expected %d items, got %d", tc.filter, tc.expected, len(filtered))
		}
		if len(filtered) > 0 {
			item := filtered[0].(map[string]any)
			if item["title"] != tc.title {
				t.Errorf("backlog.list timeframe=%s: expected Title=%q, got %v", tc.filter, tc.title, item["title"])
			}
		}
	}

	// Step 4: Export → verify all 3 appear in JSON content
	resp, err = env.Client.Send(ctx, "backlog", "export", nil, "")
	if err != nil {
		t.Fatalf("backlog.export: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.export: expected OK, got error: %s", resp.Error)
	}
	var exportData map[string]string
	if err := json.Unmarshal(resp.Data, &exportData); err != nil {
		t.Fatalf("backlog.export: unmarshal: %v", err)
	}
	content := exportData["content"]
	for _, item := range items {
		title := item["title"].(string)
		if !strings.Contains(content, title) {
			t.Errorf("backlog.export: content missing item %q", title)
		}
	}
	// JSON format should contain next_id
	if !strings.Contains(content, "next_id") {
		t.Error("backlog.export: missing 'next_id' field in JSON")
	}
}

// TestBacklog_RoundtripFidelity verifies all fields survive a full add → get roundtrip,
// and that resolve updates state correctly.
func TestBacklog_RoundtripFidelity(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Step 1: Add item with all fields populated
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id":          1,
		"title":       "Full Field Test",
		"severity":    "CRITICAL",
		"timeframe":   "NOW",
		"scope":       "GATEWAY",
		"type":        "BUG",
		"description": "All fields populated for fidelity test",
		"related":     "2,3",
	}, "")
	if err != nil {
		t.Fatalf("backlog.add: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.add: expected OK, got error: %s", resp.Error)
	}

	// Step 2: Get by id → verify every field matches
	resp, err = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	if err != nil {
		t.Fatalf("backlog.get: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.get: expected OK, got error: %s", resp.Error)
	}
	var item map[string]any
	if err := json.Unmarshal(resp.Data, &item); err != nil {
		t.Fatalf("backlog.get: unmarshal: %v", err)
	}

	checks := map[string]string{
		"title":       "Full Field Test",
		"severity":    "CRITICAL",
		"timeframe":   "NOW",
		"scope":       "GATEWAY",
		"type":        "BUG",
		"description": "All fields populated for fidelity test",
		"related":     "2,3",
		"status":      "OPEN",
	}
	for field, want := range checks {
		got, ok := item[field]
		if !ok {
			t.Errorf("backlog.get: missing field %q", field)
			continue
		}
		if got != want {
			t.Errorf("backlog.get: field %q: expected %q, got %v", field, want, got)
		}
	}

	// Step 3: Resolve the item
	resp, err = env.Client.Send(ctx, "backlog", "resolve", map[string]any{
		"id":         1,
		"resolution": "WONTFIX",
	}, "")
	if err != nil {
		t.Fatalf("backlog.resolve: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.resolve: expected OK, got error: %s", resp.Error)
	}

	// Step 4: Get again → verify resolution recorded
	resp, err = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	if err != nil {
		t.Fatalf("backlog.get after resolve: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.get after resolve: expected OK, got error: %s", resp.Error)
	}
	var resolvedItem map[string]any
	if err := json.Unmarshal(resp.Data, &resolvedItem); err != nil {
		t.Fatalf("backlog.get after resolve: unmarshal: %v", err)
	}
	if resolvedItem["status"] != "RESOLVED" {
		t.Errorf("backlog.get after resolve: expected Status='RESOLVED', got %v", resolvedItem["status"])
	}
	if resolvedItem["resolution"] != "WONTFIX" {
		t.Errorf("backlog.get after resolve: expected Resolution='WONTFIX', got %v", resolvedItem["resolution"])
	}
	if resolvedItem["resolved_at"] == "" {
		t.Error("backlog.get after resolve: expected non-empty ResolvedAt")
	}

	// Step 5: Add another item → verify id increments
	resp, err = env.Client.Send(ctx, "backlog", "next-id", nil, "")
	if err != nil {
		t.Fatalf("next-id: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("next-id: expected OK, got error: %s", resp.Error)
	}
	var nextIDData map[string]any
	if err := json.Unmarshal(resp.Data, &nextIDData); err != nil {
		t.Fatalf("next-id: unmarshal: %v", err)
	}
	// After adding id=1, next-id should return 2
	if nextIDData["id"].(float64) != 2 {
		t.Errorf("next-id: expected 2 after one item, got %v", nextIDData["id"])
	}

	resp, err = env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id":          2,
		"title":       "Second Item",
		"severity":    "MINOR",
		"timeframe":   "DEFERRED",
		"scope":       "TOOLS",
		"type":        "PERF",
		"description": "Second item to verify id increment",
	}, "")
	if err != nil {
		t.Fatalf("backlog.add second: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("backlog.add second: expected OK, got error: %s", resp.Error)
	}
	var addData map[string]any
	if err := json.Unmarshal(resp.Data, &addData); err != nil {
		t.Fatalf("backlog.add second: unmarshal: %v", err)
	}
	if addData["id"].(float64) != 2 {
		t.Errorf("backlog.add second: expected id=2, got %v", addData["id"])
	}
}

// TestBacklog_EnumValidation verifies that invalid enum values are rejected.
func TestBacklog_EnumValidation(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// 소문자 type 거부
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"title": "Bad Type", "severity": "MAJOR", "timeframe": "NOW",
		"scope": "CORE", "type": "bug", "description": "should fail",
	}, "")
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if resp.OK {
		t.Error("lowercase type 'bug' should be rejected")
	}
	if !strings.Contains(resp.Error, "invalid type") {
		t.Errorf("expected 'invalid type' error, got: %s", resp.Error)
	}

	// 소문자 scope 거부
	resp, err = env.Client.Send(ctx, "backlog", "add", map[string]any{
		"title": "Bad Scope", "severity": "MAJOR", "timeframe": "NOW",
		"scope": "core", "type": "BUG", "description": "should fail",
	}, "")
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if resp.OK {
		t.Error("lowercase scope 'core' should be rejected")
	}

	// 잘못된 resolution 거부
	resp, err = env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id": 1, "title": "OK Item", "severity": "MAJOR", "timeframe": "NOW",
		"scope": "CORE", "type": "BUG", "description": "valid item",
	}, "")
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("valid add should succeed: %s", resp.Error)
	}

	resp, err = env.Client.Send(ctx, "backlog", "resolve", map[string]any{
		"id": 1, "resolution": "DEFERRED",
	}, "")
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if resp.OK {
		t.Error("invalid resolution 'DEFERRED' should be rejected")
	}

	// 정상 resolution
	resp, err = env.Client.Send(ctx, "backlog", "resolve", map[string]any{
		"id": 1, "resolution": "FIXED",
	}, "")
	if err != nil {
		t.Fatalf("transport error: %v", err)
	}
	if !resp.OK {
		t.Errorf("valid resolution 'FIXED' should succeed: %s", resp.Error)
	}
}

// TestBacklog_Release verifies the release command flow.
func TestBacklog_Release(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// Add an item
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"id": 1, "title": "Release Test", "severity": "MAJOR", "timeframe": "NOW",
		"scope": "CORE", "type": "BUG", "description": "test release flow",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("add: %v / %s", err, resp.Error)
	}

	// Set to FIXING first (Release requires FIXING status)
	resp, err = env.Client.Send(ctx, "backlog", "fix", map[string]any{
		"id": 1, "branch": "branch_test",
	}, "")
	if err != nil || !resp.OK {
		t.Fatalf("fix: %v / %s", err, resp.Error)
	}

	// Release it
	resp, err = env.Client.Send(ctx, "backlog", "release", map[string]any{
		"id": 1, "reason": "scope change", "branch": "branch_test",
	}, "")
	if err != nil {
		t.Fatalf("release transport: %v", err)
	}
	if !resp.OK {
		t.Fatalf("release should succeed: %s", resp.Error)
	}

	// Verify description has release history appended
	resp, err = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	if err != nil || !resp.OK {
		t.Fatalf("get: %v / %s", err, resp.Error)
	}
	var item map[string]any
	if err := json.Unmarshal(resp.Data, &item); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	desc, _ := item["description"].(string)
	if !strings.Contains(desc, "[RELEASED]") {
		t.Errorf("description should contain [RELEASED], got: %s", desc)
	}
	if !strings.Contains(desc, "scope change") {
		t.Errorf("description should contain reason, got: %s", desc)
	}
}
