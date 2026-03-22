// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"

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
// Routes will be fully implemented in Task 2.
func (m *Module) RegisterRoutes(_ daemon.RouteRegistrar) {}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }
