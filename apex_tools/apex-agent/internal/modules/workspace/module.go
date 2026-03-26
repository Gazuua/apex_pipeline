// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements daemon.Module for workspace management.
type Module struct {
	store   *store.Store
	manager *Manager
	cfg     *config.WorkspaceConfig
}

// New creates a workspace Module.
func New(s *store.Store, cfg *config.WorkspaceConfig) *Module {
	return &Module{
		store:   s,
		manager: NewManager(s, cfg),
		cfg:     cfg,
	}
}

func (m *Module) Name() string      { return "workspace" }
func (m *Module) Manager() *Manager { return m.manager }

func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("workspace", 1, func(tx *store.TxStore) error {
		ctx := context.Background()
		_, err := tx.Exec(ctx, `CREATE TABLE local_branches (
			workspace_id    TEXT PRIMARY KEY,
			directory       TEXT NOT NULL UNIQUE,
			git_branch      TEXT,
			git_status      TEXT DEFAULT 'UNKNOWN',
			session_id      TEXT,
			session_pid     INTEGER DEFAULT 0,
			session_status  TEXT DEFAULT 'STOP',
			session_log     TEXT,
			last_scanned    TEXT,
			created_at      TEXT DEFAULT (datetime('now','localtime'))
		)`)
		return err
	})
}

func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("scan", daemon.NoParams(m.handleScan))
	reg.Handle("list", daemon.NoParams(m.handleList))
	reg.Handle("get", m.handleGet)
	reg.Handle("sync", m.handleSync)
}

func (m *Module) OnStart(ctx context.Context) error {
	if m.cfg != nil && m.cfg.ScanOnStart && m.cfg.Root != "" {
		if _, err := m.manager.Scan(ctx); err != nil {
			return fmt.Errorf("workspace scan on start: %w", err)
		}
	}
	return nil
}

func (m *Module) OnStop() error { return nil }

// --- IPC handlers ---

func (m *Module) handleScan(ctx context.Context, _ string) (any, error) {
	return m.manager.Scan(ctx)
}

func (m *Module) handleList(ctx context.Context, _ string) (any, error) {
	return m.manager.List(ctx)
}

func (m *Module) handleGet(ctx context.Context, params json.RawMessage, _ string) (any, error) {
	var p struct {
		WorkspaceID string `json:"workspace_id"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("parse get params: %w", err)
	}
	return m.manager.Get(ctx, p.WorkspaceID)
}

func (m *Module) handleSync(ctx context.Context, params json.RawMessage, _ string) (any, error) {
	var p struct {
		WorkspaceID string `json:"workspace_id"`
		All         bool   `json:"all"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("parse sync params: %w", err)
	}
	if p.All {
		branches, err := m.manager.List(ctx)
		if err != nil {
			return nil, err
		}
		type syncResult struct {
			ID     string `json:"id"`
			OK     bool   `json:"ok"`
			Output string `json:"output"`
		}
		var results []syncResult
		for _, b := range branches {
			out, err := m.manager.SyncBranch(ctx, b.WorkspaceID)
			results = append(results, syncResult{ID: b.WorkspaceID, OK: err == nil, Output: out})
		}
		return map[string]any{"results": results}, nil
	}
	out, err := m.manager.SyncBranch(ctx, p.WorkspaceID)
	if err != nil {
		return map[string]any{"ok": false, "output": out}, err
	}
	return map[string]any{"ok": true, "output": out}, nil
}
