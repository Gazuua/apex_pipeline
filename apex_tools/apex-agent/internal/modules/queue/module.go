// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package queue

import (
	"context"
	"os"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for queue/lock management.
type Module struct {
	manager *Manager
}

// New creates a new queue Module backed by the given store.
func New(s *store.Store) *Module {
	return &Module{manager: NewManager(s)}
}

func (m *Module) Name() string { return "queue" }

// Manager returns the underlying Manager for cross-module use.
func (m *Module) Manager() *Manager { return m.manager }

// RegisterSchema registers the queue table migration.
func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("queue", 1, func(tx *store.TxStore) error {
		ctx := context.Background()
		_, err := tx.Exec(ctx, `CREATE TABLE queue (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			channel    TEXT    NOT NULL,
			branch     TEXT    NOT NULL,
			pid        INTEGER NOT NULL,
			status     TEXT    NOT NULL DEFAULT 'WAITING',
			created_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`)
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `CREATE INDEX idx_queue_channel_status ON queue(channel, status)`)
		return err
	})
	mig.Register("queue", 2, func(tx *store.TxStore) error {
		_, err := tx.Exec(context.Background(), `UPDATE queue SET status = UPPER(status)`)
		return err
	})
	mig.Register("queue", 3, func(tx *store.TxStore) error {
		_, err := tx.Exec(context.Background(), `ALTER TABLE queue ADD COLUMN finished_at TEXT`)
		return err
	})
	mig.Register("queue", 4, func(tx *store.TxStore) error {
		ctx := context.Background()
		_, err := tx.Exec(ctx, `CREATE TABLE queue_history (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			channel    TEXT NOT NULL,
			branch     TEXT NOT NULL,
			status     TEXT NOT NULL,
			timestamp  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`)
		if err != nil {
			return err
		}
		_, err = tx.Exec(ctx, `CREATE INDEX idx_queue_history_channel_ts ON queue_history(channel, timestamp DESC)`)
		return err
	})
}

// RegisterRoutes registers all queue action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("acquire", daemon.Typed(m.handleAcquire))
	reg.Handle("try-acquire", daemon.Typed(m.handleTryAcquire))
	reg.Handle("release", daemon.Typed(m.handleRelease))
	reg.Handle("status", daemon.Typed(m.handleStatus))
	reg.Handle("cleanup-stale", daemon.NoParams(m.handleCleanupStale))
	reg.Handle("update-pid", daemon.Typed(m.handleUpdatePID))
}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }

// --- Request types ---

type acquireParams struct {
	Channel string `json:"channel"`
	Branch  string `json:"branch"`
	PID     int    `json:"pid"`
}

type releaseParams struct {
	Channel string `json:"channel"`
}

type statusParams struct {
	Channel string `json:"channel"`
}

// --- Handlers ---

func (m *Module) handleAcquire(reqCtx context.Context, p acquireParams, _ string) (any, error) {
	pid := p.PID
	if pid == 0 {
		pid = os.Getpid()
	}
	// 30분 timeout + server shutdown(reqCtx) 시 자동 취소
	ctx, cancel := context.WithTimeout(reqCtx, 30*time.Minute)
	defer cancel()
	if err := m.manager.Acquire(ctx, p.Channel, p.Branch, pid); err != nil {
		return nil, err
	}
	return map[string]any{"acquired": true, "channel": p.Channel}, nil
}

func (m *Module) handleTryAcquire(ctx context.Context, p acquireParams, _ string) (any, error) {
	pid := p.PID
	if pid == 0 {
		pid = os.Getpid()
	}
	acquired, err := m.manager.TryAcquire(ctx, p.Channel, p.Branch, pid)
	if err != nil {
		return nil, err
	}
	return map[string]any{"acquired": acquired, "channel": p.Channel}, nil
}

func (m *Module) handleRelease(ctx context.Context, p releaseParams, _ string) (any, error) {
	if err := m.manager.Release(ctx, p.Channel); err != nil {
		return nil, err
	}
	return map[string]any{"released": true, "channel": p.Channel}, nil
}

func (m *Module) handleStatus(ctx context.Context, p statusParams, _ string) (any, error) {
	active, waiting, err := m.manager.Status(ctx, p.Channel)
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"channel": p.Channel,
		"active":  active,
		"waiting": waiting,
	}, nil
}

type updatePIDParams struct {
	Channel string `json:"channel"`
	Branch  string `json:"branch"`
	PID     int    `json:"pid"`
}

func (m *Module) handleUpdatePID(ctx context.Context, p updatePIDParams, _ string) (any, error) {
	if err := m.manager.UpdatePID(ctx, p.Channel, p.Branch, p.PID); err != nil {
		return nil, err
	}
	return map[string]any{"updated": true, "channel": p.Channel, "pid": p.PID}, nil
}

func (m *Module) handleCleanupStale(ctx context.Context, _ string) (any, error) {
	removed, err := m.manager.CleanupStale(ctx)
	if err != nil {
		return nil, err
	}
	return map[string]any{"removed": removed}, nil
}
