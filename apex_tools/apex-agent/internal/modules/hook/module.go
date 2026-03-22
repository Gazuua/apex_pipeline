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

func (m *Module) handleValidateMerge(ctx context.Context, params json.RawMessage, workspace string) (any, error) {
	var p struct {
		Command string `json:"command"`
		Cwd     string `json:"cwd"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, err
	}
	if err := ValidateMerge(p.Command, p.Cwd); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}
