// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"
	"fmt"
)

// Typed wraps a strongly-typed handler into a HandlerFunc.
// It handles JSON unmarshaling of params, eliminating boilerplate.
func Typed[P any](fn func(ctx context.Context, p P, workspace string) (any, error)) HandlerFunc {
	return func(ctx context.Context, params json.RawMessage, workspace string) (any, error) {
		var p P
		if err := json.Unmarshal(params, &p); err != nil {
			return nil, fmt.Errorf("decode params: %w", err)
		}
		return fn(ctx, p, workspace)
	}
}

// NoParams wraps a handler that takes no params into a HandlerFunc.
// The params argument is ignored.
func NoParams(fn func(ctx context.Context, workspace string) (any, error)) HandlerFunc {
	return func(ctx context.Context, _ json.RawMessage, workspace string) (any, error) {
		return fn(ctx, workspace)
	}
}
