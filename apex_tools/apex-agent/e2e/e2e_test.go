// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package e2e

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/e2e/testenv"
)

func TestE2E_FullRoundtrip(t *testing.T) {
	env := testenv.New(t)

	// Send request via client.
	ctx := context.Background()
	resp, err := env.Client.Send(ctx, "daemon", "version", nil, "")
	if err != nil {
		t.Fatalf("client.Send: %v", err)
	}
	if !resp.OK {
		t.Fatalf("response not OK: %s", resp.Error)
	}

	var data map[string]string
	json.Unmarshal(resp.Data, &data)
	if _, ok := data["version"]; !ok {
		t.Error("version response missing 'version' field")
	}

	// Unknown module returns error.
	resp2, err := env.Client.Send(ctx, "nonexistent", "action", nil, "ws")
	if err != nil {
		t.Fatalf("Send to nonexistent module: %v", err)
	}
	if resp2.OK {
		t.Error("expected error for unknown module")
	}
}
