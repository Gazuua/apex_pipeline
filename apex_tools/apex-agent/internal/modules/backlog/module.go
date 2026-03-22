// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for backlog management.
type Module struct {
	manager *Manager
}

// New creates a new backlog Module backed by the given store.
func New(s *store.Store) *Module {
	return &Module{manager: NewManager(s)}
}

func (m *Module) Name() string { return "backlog" }

// RegisterSchema registers the backlog_items table migration.
func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("backlog", 1, func(s *store.Store) error {
		_, err := s.Exec(`CREATE TABLE backlog_items (
			id          INTEGER PRIMARY KEY,
			title       TEXT    NOT NULL,
			severity    TEXT    NOT NULL,
			timeframe   TEXT    NOT NULL,
			scope       TEXT    NOT NULL,
			type        TEXT    NOT NULL,
			description TEXT    NOT NULL,
			related     TEXT,
			position    INTEGER NOT NULL,
			status      TEXT    NOT NULL DEFAULT 'open',
			resolution  TEXT,
			resolved_at TEXT,
			created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`)
		return err
	})
}

// RegisterRoutes registers all backlog action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("add", m.handleAdd)
	reg.Handle("list", m.handleList)
	reg.Handle("get", m.handleGet)
	reg.Handle("resolve", m.handleResolve)
	reg.Handle("check", m.handleCheck)
	reg.Handle("next-id", m.handleNextID)
	reg.Handle("export", m.handleExport)
}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }

// ── Route handlers ──

func (m *Module) handleAdd(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var item BacklogItem
	if err := json.Unmarshal(params, &item); err != nil {
		return nil, fmt.Errorf("backlog.add: decode params: %w", err)
	}
	if err := m.manager.Add(&item); err != nil {
		return nil, err
	}
	return map[string]any{"id": item.ID, "position": item.Position}, nil
}

func (m *Module) handleList(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var filter ListFilter
	if len(params) > 0 && string(params) != "null" {
		if err := json.Unmarshal(params, &filter); err != nil {
			return nil, fmt.Errorf("backlog.list: decode params: %w", err)
		}
	}
	items, err := m.manager.List(filter)
	if err != nil {
		return nil, err
	}
	return items, nil
}

func (m *Module) handleGet(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p struct {
		ID int `json:"id"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("backlog.get: decode params: %w", err)
	}
	item, err := m.manager.Get(p.ID)
	if err != nil {
		return nil, err
	}
	return item, nil
}

func (m *Module) handleResolve(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p struct {
		ID         int    `json:"id"`
		Resolution string `json:"resolution"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("backlog.resolve: decode params: %w", err)
	}
	if err := m.manager.Resolve(p.ID, p.Resolution); err != nil {
		return nil, err
	}
	return map[string]string{"status": "resolved"}, nil
}

func (m *Module) handleCheck(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p struct {
		ID int `json:"id"`
	}
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("backlog.check: decode params: %w", err)
	}
	exists, status, err := m.manager.Check(p.ID)
	if err != nil {
		return nil, err
	}
	return map[string]any{"exists": exists, "status": status}, nil
}

func (m *Module) handleNextID(_ context.Context, _ json.RawMessage, _ string) (any, error) {
	id, err := m.manager.NextID()
	if err != nil {
		return nil, err
	}
	return map[string]int{"id": id}, nil
}

func (m *Module) handleExport(_ context.Context, _ json.RawMessage, _ string) (any, error) {
	content, err := m.manager.Export()
	if err != nil {
		return nil, err
	}
	return map[string]string{"content": content}, nil
}
