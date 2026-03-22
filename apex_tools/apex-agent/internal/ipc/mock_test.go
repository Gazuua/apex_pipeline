// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"context"
	"encoding/json"
	"fmt"
)

// mockDispatcher is a simple Dispatcher for testing without importing daemon.
type mockDispatcher struct {
	handlers map[string]func(ctx context.Context, params json.RawMessage, ws string) (any, error)
}

func (d *mockDispatcher) Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error) {
	key := module + "." + action
	h, ok := d.handlers[key]
	if !ok {
		return nil, fmt.Errorf("unknown: %s", key)
	}
	return h(ctx, params, workspace)
}
