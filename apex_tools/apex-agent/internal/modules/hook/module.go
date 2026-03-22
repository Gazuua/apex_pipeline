// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"context"
	"encoding/json"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for hook validation.
type Module struct{}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "hook" }

func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("validate-build", m.handleValidateBuild)
	reg.Handle("validate-merge", m.handleValidateMerge)
}

func (m *Module) RegisterSchema(mig *store.Migrator) {
	// Hook module has no DB schema (stateless validation).
}

func (m *Module) OnStart(ctx context.Context) error { return nil }
func (m *Module) OnStop() error                     { return nil }

func (m *Module) handleValidateBuild(ctx context.Context, params json.RawMessage, workspace string) (any, error) {
	var p struct {
		Command string `json:"command"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, err
	}
	if err := ValidateBuild(p.Command); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

// handleValidateMerge is a no-op stub — merge lock validation is now handled
// by CLI hook_cmd.go via daemon IPC (queue.status). Kept for route compatibility.
func (m *Module) handleValidateMerge(_ context.Context, _ json.RawMessage, _ string) (any, error) {
	return map[string]string{"status": "allowed"}, nil
}
