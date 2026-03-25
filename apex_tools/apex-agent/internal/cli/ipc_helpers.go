// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

var (
	defaultClient     *ipc.Client
	defaultClientOnce sync.Once
)

func getClient() *ipc.Client {
	defaultClientOnce.Do(func() {
		defaultClient = ipc.NewClient(platform.SocketPath())
	})
	return defaultClient
}

// sendWithAutoRestart sends an IPC request; on dial failure, auto-starts the
// daemon and retries once. This ensures hooks and CLI commands recover seamlessly
// when the daemon is not running (e.g., after a crash or manual stop).
func sendWithAutoRestart(ctx context.Context, module, action string, params any, workspace string) (*ipc.Response, error) {
	resp, err := getClient().Send(ctx, module, action, params, workspace)
	if err != nil && isDialError(err) {
		ensureDaemon()
		resp, err = getClient().Send(ctx, module, action, params, workspace)
	}
	return resp, err
}

// isDialError checks if the error is a connection failure (daemon unreachable).
func isDialError(err error) bool {
	return err != nil && strings.Contains(err.Error(), "dial:")
}

// sendRequest sends an IPC request to the daemon and returns the raw response.
// Auto-restarts daemon on connection failure. For long-running operations, use sendRequestWithTimeout.
func sendRequest(module, action string, params any, workspace string) (*ipc.Response, error) {
	return sendWithAutoRestart(context.Background(), module, action, params, workspace)
}

// sendRequestWithTimeout sends an IPC request with a custom timeout.
// Auto-restarts daemon on connection failure.
// Use for long-running operations (e.g., queue acquire that may block up to 30min).
func sendRequestWithTimeout(module, action string, params any, workspace string, timeout time.Duration) (*ipc.Response, error) {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	return sendWithAutoRestart(ctx, module, action, params, workspace)
}

// sendRequestMap sends an IPC request and parses the response Data as map[string]any.
func sendRequestMap(module, action string, params any, workspace string) (map[string]any, error) {
	resp, err := sendRequest(module, action, params, workspace)
	if err != nil {
		return nil, err
	}
	return parseResponseMap(resp)
}

// sendRequestMapWithTimeout sends an IPC request with a custom timeout and
// parses the response Data as map[string]any.
func sendRequestMapWithTimeout(module, action string, params any, workspace string, timeout time.Duration) (map[string]any, error) {
	resp, err := sendRequestWithTimeout(module, action, params, workspace, timeout)
	if err != nil {
		return nil, err
	}
	return parseResponseMap(resp)
}

// parseResponseMap extracts the response Data as map[string]any, checking for errors.
func parseResponseMap(resp *ipc.Response) (map[string]any, error) {
	if resp.Error != "" {
		return nil, fmt.Errorf("%s", resp.Error)
	}
	var result map[string]any
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		return nil, fmt.Errorf("decode response: %w", err)
	}
	return result, nil
}
