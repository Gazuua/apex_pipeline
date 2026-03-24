// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package dispatch

import (
	"context"
	"encoding/json"
)

// Dispatcher routes requests to module handlers.
type Dispatcher interface {
	Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error)
}
