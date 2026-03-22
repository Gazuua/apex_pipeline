// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"fmt"
)

// Client sends IPC requests to the daemon and returns responses.
type Client struct {
	addr string
}

// NewClient creates a new IPC client targeting the given address.
func NewClient(addr string) *Client {
	return &Client{addr: addr}
}

// Send dials the daemon, sends a request, and reads the response.
// Each call opens and closes its own connection (stateless).
func (c *Client) Send(ctx context.Context, module, action string, params any, workspace string) (*Response, error) {
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
