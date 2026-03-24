// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/version"
)

// Client sends IPC requests to the daemon and returns responses.
type Client struct {
	addr        string
	versionOnce sync.Once
}

// NewClient creates a new IPC client targeting the given address.
func NewClient(addr string) *Client {
	return &Client{addr: addr}
}

// Send checks the daemon version on the first call (unless version.Version == "dev"),
// then sends the request and returns the response.
// Each call opens and closes its own connection (stateless).
func (c *Client) Send(ctx context.Context, module, action string, params any, workspace string) (*Response, error) {
	c.versionOnce.Do(c.checkVersion)
	return c.send(ctx, module, action, params, workspace)
}

// send is the low-level transport method used by both Send and checkVersion.
func (c *Client) send(ctx context.Context, module, action string, params any, workspace string) (*Response, error) {
	var rawParams json.RawMessage
	if params != nil {
		var err error
		rawParams, err = json.Marshal(params)
		if err != nil {
			return nil, fmt.Errorf("marshal params: %w", err)
		}
	}

	conn, err := Dial(c.addr)
	if err != nil {
		return nil, fmt.Errorf("dial: %w", err)
	}
	defer conn.Close()

	if deadline, ok := ctx.Deadline(); ok {
		conn.SetDeadline(deadline)
	} else {
		conn.SetDeadline(time.Now().Add(30 * time.Second))
	}

	req := &Request{
		Module:    module,
		Action:    action,
		Params:    rawParams,
		Workspace: workspace,
	}

	if err := WriteMessage(conn, req); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}

	var resp Response
	if err := ReadMessage(conn, &resp); err != nil {
		return nil, fmt.Errorf("read: %w", err)
	}

	return &resp, nil
}

// checkVersion queries the daemon version and triggers a graceful shutdown if
// there is a mismatch. The caller (run-hook wrapper or CLI daemon_helpers)
// is responsible for auto-starting the daemon on subsequent dial failures.
func (c *Client) checkVersion() {
	if version.Version == "dev" {
		return
	}

	resp, err := c.send(context.Background(), "daemon", "version", nil, "")
	if err != nil {
		// Daemon not running — auto-start will handle this.
		return
	}

	var data map[string]string
	if err := json.Unmarshal(resp.Data, &data); err != nil {
		return
	}

	if data["version"] != version.Version {
		ml.Info("version mismatch, restarting daemon",
			"cli", version.Version,
			"daemon", data["version"],
		)
		// Ask daemon to shut down gracefully; ignore errors (best effort).
		_, _ = c.send(context.Background(), "daemon", "shutdown", nil, "")
		// Give the daemon a moment to exit before the next dial attempt.
		time.Sleep(500 * time.Millisecond)
	}
}
