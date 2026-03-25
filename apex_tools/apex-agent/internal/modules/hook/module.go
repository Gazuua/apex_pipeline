// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package hook

import (
	"context"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for hook validation.
type Module struct{}

func New() *Module { return &Module{} }

func (m *Module) Name() string { return "hook" }

// validateBuildParams holds params for the validate-build action.
type validateBuildParams struct {
	Command string `json:"command"`
}

func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("validate-build", daemon.Typed(m.handleValidateBuild))
	reg.Handle("validate-merge", daemon.NoParams(m.handleValidateMerge))
}

func (m *Module) RegisterSchema(mig *store.Migrator) {
	// Hook module has no DB schema (stateless validation).
}

func (m *Module) OnStart(ctx context.Context) error { return nil }
func (m *Module) OnStop() error                     { return nil }

func (m *Module) handleValidateBuild(_ context.Context, p validateBuildParams, _ string) (any, error) {
	if err := ValidateBuild(p.Command); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

// handleValidateMerge is a no-op stub — merge lock validation is now handled
// by CLI hook_cmd.go via daemon IPC (queue.status). Kept for route compatibility.
func (m *Module) handleValidateMerge(_ context.Context, _ string) (any, error) {
	return map[string]string{"status": "allowed"}, nil
}
