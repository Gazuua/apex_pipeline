// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package httpd

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"
)

func TestServer_StartStop(t *testing.T) {
	srv := New(nil, nil, "localhost:0")
	if err := srv.Start(); err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	defer srv.Stop()

	resp, err := http.Get("http://" + srv.Addr() + "/health")
	if err != nil {
		t.Fatalf("GET /health failed: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var body map[string]any
	json.NewDecoder(resp.Body).Decode(&body)
	if body["ok"] != true {
		t.Fatalf("expected ok=true, got %v", body["ok"])
	}
}

func TestServer_LastRequestTime(t *testing.T) {
	srv := New(nil, nil, "localhost:0")
	if err := srv.Start(); err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	defer srv.Stop()

	// Before any request, lastRequest should be 0
	if srv.LastRequestTime() != 0 {
		t.Fatalf("expected 0 before any request, got %d", srv.LastRequestTime())
	}

	before := time.Now().Unix()
	resp, err := http.Get("http://" + srv.Addr() + "/health")
	if err != nil {
		t.Fatalf("GET /health failed: %v", err)
	}
	resp.Body.Close()
	after := time.Now().Unix()

	last := srv.LastRequestTime()
	if last < before || last > after {
		t.Fatalf("LastRequestTime %d not in [%d, %d]", last, before, after)
	}
}

func TestServer_StopIdempotent(t *testing.T) {
	srv := New(nil, nil, "localhost:0")
	if err := srv.Start(); err != nil {
		t.Fatalf("Start failed: %v", err)
	}
	if err := srv.Stop(); err != nil {
		t.Fatalf("first Stop failed: %v", err)
	}
	// Second stop should not panic
	srv.Stop()
}
