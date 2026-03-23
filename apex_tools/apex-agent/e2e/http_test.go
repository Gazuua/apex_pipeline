// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

func TestHTTP_HealthCheck(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/health")
	if err != nil {
		t.Fatalf("GET /health: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var body map[string]any
	json.NewDecoder(resp.Body).Decode(&body)
	if body["ok"] != true {
		t.Errorf("expected ok=true, got %v", body)
	}
}

func TestHTTP_DashboardPage(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/")
	if err != nil {
		t.Fatalf("GET /: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "text/html") {
		t.Errorf("expected text/html, got %s", ct)
	}

	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), "Dashboard") {
		t.Error("response doesn't contain 'Dashboard'")
	}
	if !strings.Contains(string(body), "htmx.min.js") {
		t.Error("response doesn't contain htmx script reference")
	}
}

func TestHTTP_BacklogPage(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/backlog")
	if err != nil {
		t.Fatalf("GET /backlog: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}

func TestHTTP_HandoffPage(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/handoff")
	if err != nil {
		t.Fatalf("GET /handoff: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}

func TestHTTP_QueuePage(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/queue")
	if err != nil {
		t.Fatalf("GET /queue: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}

func TestHTTP_BacklogAPI(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	// Add a backlog item via IPC
	_, err := env.Client.Send(t.Context(), "backlog", "add", map[string]any{
		"title":       "test item",
		"severity":    "MAJOR",
		"timeframe":   "NOW",
		"scope":       "CORE",
		"type":        "BUG",
		"description": "test description",
	}, "")
	if err != nil {
		t.Fatalf("backlog add: %v", err)
	}

	// Fetch via HTTP JSON API
	resp, err := http.Get("http://" + env.HTTPAddr + "/api/backlog")
	if err != nil {
		t.Fatalf("GET /api/backlog: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var result map[string]any
	json.NewDecoder(resp.Body).Decode(&result)
	if result["ok"] != true {
		t.Errorf("expected ok=true, got %v", result)
	}

	data, ok := result["data"].([]any)
	if !ok || len(data) == 0 {
		t.Errorf("expected non-empty data array, got %v", result["data"])
	}
}

func TestHTTP_HandoffAPI(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/api/handoff")
	if err != nil {
		t.Fatalf("GET /api/handoff: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var result map[string]any
	json.NewDecoder(resp.Body).Decode(&result)
	if result["ok"] != true {
		t.Errorf("expected ok=true, got %v", result)
	}
}

func TestHTTP_QueueAPI(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/api/queue")
	if err != nil {
		t.Fatalf("GET /api/queue: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var result map[string]any
	json.NewDecoder(resp.Body).Decode(&result)
	if result["ok"] != true {
		t.Errorf("expected ok=true, got %v", result)
	}
}

func TestHTTP_StaticFiles(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/static/style.css")
	if err != nil {
		t.Fatalf("GET /static/style.css: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
}

func TestHTTP_NotFound(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/nonexistent")
	if err != nil {
		t.Fatalf("GET /nonexistent: %v", err)
	}
	defer resp.Body.Close()

	// "/" catch-all returns 404 for unknown paths
	if resp.StatusCode != 404 {
		t.Errorf("expected 404, got %d", resp.StatusCode)
	}
}

func TestHTTP_PartialSummary(t *testing.T) {
	env := testenv.New(t)
	defer env.Stop()

	resp, err := http.Get("http://" + env.HTTPAddr + "/partials/summary")
	if err != nil {
		t.Fatalf("GET /partials/summary: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "text/html") {
		t.Errorf("expected text/html, got %s", ct)
	}
}
