// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package daemon

import (
	"context"
	"encoding/json"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// HandlerFunc processes a module action.
type HandlerFunc func(ctx context.Context, params json.RawMessage, workspace string) (any, error)

// RouteRegistrar allows a module to register its action handlers.
type RouteRegistrar interface {
	Handle(action string, handler HandlerFunc)
}

// Module is the interface that all apex-agent modules implement.
type Module interface {
	Name() string
	RegisterRoutes(reg RouteRegistrar)
	RegisterSchema(m *store.Migrator)
	OnStart(ctx context.Context) error
	OnStop() error
}
