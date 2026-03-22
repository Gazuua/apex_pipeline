// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for handoff management.
type Module struct {
	manager *Manager
}

// New creates a new handoff Module backed by the given store.
func New(s *store.Store) *Module {
	return &Module{manager: NewManager(s)}
}

func (m *Module) Name() string { return "handoff" }

// RegisterSchema registers the branches, notifications, and notification_acks table migrations.
func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("handoff", 1, func(s *store.Store) error {
		queries := []string{
			`CREATE TABLE branches (
				branch      TEXT    PRIMARY KEY,
				workspace   TEXT    NOT NULL,
				status      TEXT    NOT NULL,
				backlog_id  INTEGER,
				summary     TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			)`,
			`CREATE TABLE notifications (
				id          INTEGER PRIMARY KEY AUTOINCREMENT,
				branch      TEXT    NOT NULL,
				workspace   TEXT    NOT NULL,
				type        TEXT    NOT NULL,
				summary     TEXT,
				payload     TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			)`,
			`CREATE TABLE notification_acks (
				notification_id INTEGER NOT NULL,
				branch          TEXT    NOT NULL,
				action          TEXT    NOT NULL,
				acked_at        TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				PRIMARY KEY (notification_id, branch)
			)`,
			`CREATE INDEX idx_notifications_branch ON notifications(branch)`,
			`CREATE INDEX idx_acks_branch ON notification_acks(branch)`,
		}
		for _, q := range queries {
			if _, err := s.Exec(q); err != nil {
				return err
			}
		}
		return nil
	})
}

// RegisterRoutes registers handoff action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("notify-start", m.handleNotifyStart)
	reg.Handle("notify-transition", m.handleNotifyTransition)
	reg.Handle("check", m.handleCheck)
	reg.Handle("ack", m.handleAck)
	reg.Handle("backlog-check", m.handleBacklogCheck)
	reg.Handle("get-branch", m.handleGetBranch)
	reg.Handle("get-status", m.handleGetStatus)
}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }

// --- Request/Response types ---

type notifyStartParams struct {
	Branch     string `json:"branch"`
	Workspace  string `json:"workspace"`
	Summary    string `json:"summary"`
	BacklogID  int    `json:"backlog_id"`
	Scopes     string `json:"scopes"`
	SkipDesign bool   `json:"skip_design"`
}

type notifyTransitionParams struct {
	Branch    string `json:"branch"`
	Workspace string `json:"workspace"`
	Type      string `json:"type"`
	Summary   string `json:"summary"`
}

type checkParams struct {
	Branch string `json:"branch"`
}

type ackParams struct {
	NotificationID int    `json:"notification_id"`
	Branch         string `json:"branch"`
	Action         string `json:"action"`
}

type backlogCheckParams struct {
	BacklogID int `json:"backlog_id"`
}

type getBranchParams struct {
	Branch string `json:"branch"`
}

type getStatusParams struct {
	Branch string `json:"branch"`
}

// --- Handlers ---

func (m *Module) handleNotifyStart(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyStartParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	id, err := m.manager.NotifyStart(p.Branch, p.Workspace, p.Summary, p.BacklogID, p.Scopes, p.SkipDesign)
	if err != nil {
		return nil, err
	}
	return map[string]any{"notification_id": id}, nil
}

func (m *Module) handleNotifyTransition(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyTransitionParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	id, err := m.manager.NotifyTransition(p.Branch, p.Workspace, p.Type, p.Summary)
	if err != nil {
		return nil, err
	}
	return map[string]any{"notification_id": id}, nil
}

func (m *Module) handleCheck(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p checkParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	notifs, err := m.manager.CheckNotifications(p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"notifications": notifs}, nil
}

func (m *Module) handleAck(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p ackParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.Ack(p.NotificationID, p.Branch, p.Action); err != nil {
		return nil, err
	}
	return map[string]any{"ok": true}, nil
}

func (m *Module) handleBacklogCheck(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p backlogCheckParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	available, branch, err := m.manager.BacklogCheck(p.BacklogID)
	if err != nil {
		return nil, err
	}
	return map[string]any{"available": available, "branch": branch}, nil
}

func (m *Module) handleGetBranch(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p getBranchParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	b, err := m.manager.GetBranch(p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"branch": b}, nil
}

func (m *Module) handleGetStatus(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p getStatusParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	status, err := m.manager.GetStatus(p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"status": status}, nil
}
