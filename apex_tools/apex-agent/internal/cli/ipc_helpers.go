// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// sendRequest sends an IPC request to the daemon and returns the raw response.
// Uses default 10-second timeout. For long-running operations, use sendRequestWithTimeout.
func sendRequest(module, action string, params any, workspace string) (*ipc.Response, error) {
	client := ipc.NewClient(platform.SocketPath())
	return client.Send(context.Background(), module, action, params, workspace)
}

// sendRequestWithTimeout sends an IPC request with a custom timeout.
// Use for long-running operations (e.g., queue acquire that may block up to 30min).
func sendRequestWithTimeout(module, action string, params any, workspace string, timeout time.Duration) (*ipc.Response, error) {
	client := ipc.NewClient(platform.SocketPath())
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	return client.Send(ctx, module, action, params, workspace)
}

// sendRequestMap sends an IPC request and parses the response Data as map[string]any.
func sendRequestMap(module, action string, params any, workspace string) (map[string]any, error) {
	resp, err := sendRequest(module, action, params, workspace)
	if err != nil {
		return nil, err
	}
	if resp.Error != "" {
		return nil, fmt.Errorf("%s", resp.Error)
	}
	var result map[string]any
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		return nil, fmt.Errorf("decode response: %w", err)
	}
	return result, nil
}
