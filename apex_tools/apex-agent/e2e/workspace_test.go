// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/workspace"
)

func TestWorkspace_ScanAndList(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// scan (config root가 비어있으니 빈 결과)
	resp, err := env.Client.Send(ctx, "workspace", "scan", nil, "")
	if err != nil {
		t.Fatalf("scan: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("scan failed: %s", resp.Error)
	}
	var scanResult workspace.ScanResult
	json.Unmarshal(resp.Data, &scanResult)
	if scanResult.Added != 0 {
		t.Errorf("want added=0 (empty root), got %d", scanResult.Added)
	}

	// list (빈 결과)
	resp, err = env.Client.Send(ctx, "workspace", "list", nil, "")
	if err != nil {
		t.Fatalf("list: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("list failed: %s", resp.Error)
	}
	var branches []workspace.LocalBranch
	json.Unmarshal(resp.Data, &branches)
	if len(branches) != 0 {
		t.Errorf("want 0 branches, got %d", len(branches))
	}
}

func TestBacklog_BlockedReason_E2E(t *testing.T) {
	env := testenv.New(t)
	ctx := context.Background()

	// backlog 항목 추가
	resp, err := env.Client.Send(ctx, "backlog", "add", map[string]any{
		"title": "test blocked", "severity": "MAJOR", "timeframe": "IN_VIEW",
		"scope": "TOOLS", "type": "INFRA", "description": "test blocked_reason",
	}, "")
	if err != nil {
		t.Fatalf("add: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("add failed: %s", resp.Error)
	}

	// blocked 설정
	resp, err = env.Client.Send(ctx, "backlog", "update", map[string]any{
		"id":     1,
		"fields": map[string]string{"blocked": "정책 결정 필요"},
	}, "")
	if err != nil {
		t.Fatalf("update blocked: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("update blocked failed: %s", resp.Error)
	}

	// get으로 확인
	resp, err = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	if err != nil {
		t.Fatalf("get: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("get failed: %s", resp.Error)
	}
	var item struct {
		BlockedReason string `json:"blocked_reason"`
	}
	json.Unmarshal(resp.Data, &item)
	if item.BlockedReason != "정책 결정 필요" {
		t.Errorf("want blocked_reason=정책 결정 필요, got %q", item.BlockedReason)
	}

	// blocked 해제
	resp, err = env.Client.Send(ctx, "backlog", "update", map[string]any{
		"id":     1,
		"fields": map[string]string{"blocked": ""},
	}, "")
	if err != nil {
		t.Fatalf("clear blocked: transport error: %v", err)
	}
	if !resp.OK {
		t.Fatalf("clear blocked failed: %s", resp.Error)
	}

	resp, _ = env.Client.Send(ctx, "backlog", "get", map[string]any{"id": 1}, "")
	var item2 struct {
		BlockedReason string `json:"blocked_reason"`
	}
	json.Unmarshal(resp.Data, &item2)
	if item2.BlockedReason != "" {
		t.Errorf("want empty blocked_reason after clear, got %q", item.BlockedReason)
	}
}
