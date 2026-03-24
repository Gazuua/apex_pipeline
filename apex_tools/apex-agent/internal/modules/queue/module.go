// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package queue

import (
	"context"
	"encoding/json"
	"fmt"
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
		_, err := tx.Exec(`CREATE TABLE queue (
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
		_, err = tx.Exec(`CREATE INDEX idx_queue_channel_status ON queue(channel, status)`)
		return err
	})
	mig.Register("queue", 2, func(tx *store.TxStore) error {
		_, err := tx.Exec(`UPDATE queue SET status = UPPER(status)`)
		return err
	})
	mig.Register("queue", 3, func(tx *store.TxStore) error {
		_, err := tx.Exec(`ALTER TABLE queue ADD COLUMN finished_at TEXT`)
		return err
	})
	mig.Register("queue", 4, func(tx *store.TxStore) error {
		_, err := tx.Exec(`CREATE TABLE queue_history (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			channel    TEXT NOT NULL,
			branch     TEXT NOT NULL,
			status     TEXT NOT NULL,
			timestamp  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`)
		if err != nil {
			return err
		}
		_, err = tx.Exec(`CREATE INDEX idx_queue_history_channel_ts ON queue_history(channel, timestamp DESC)`)
		return err
	})
}

// RegisterRoutes registers all queue action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("acquire", m.handleAcquire)
	reg.Handle("try-acquire", m.handleTryAcquire)
	reg.Handle("release", m.handleRelease)
	reg.Handle("status", m.handleStatus)
	reg.Handle("cleanup-stale", m.handleCleanupStale)
	reg.Handle("update-pid", m.handleUpdatePID)
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

func (m *Module) handleAcquire(reqCtx context.Context, params json.RawMessage, _ string) (any, error) {
	var p acquireParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("queue.acquire: decode params: %w", err)
	}
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

func (m *Module) handleTryAcquire(ctx context.Context, params json.RawMessage, _ string) (any, error) {
	var p acquireParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("queue.try-acquire: decode params: %w", err)
	}
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

func (m *Module) handleRelease(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p releaseParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("queue.release: decode params: %w", err)
	}
	if err := m.manager.Release(p.Channel); err != nil {
		return nil, err
	}
	return map[string]any{"released": true, "channel": p.Channel}, nil
}

func (m *Module) handleStatus(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p statusParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("queue.status: decode params: %w", err)
	}
	active, waiting, err := m.manager.Status(p.Channel)
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

func (m *Module) handleUpdatePID(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p updatePIDParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("queue.update-pid: decode params: %w", err)
	}
	if err := m.manager.UpdatePID(p.Channel, p.Branch, p.PID); err != nil {
		return nil, err
	}
	return map[string]any{"updated": true, "channel": p.Channel, "pid": p.PID}, nil
}

func (m *Module) handleCleanupStale(_ context.Context, _ json.RawMessage, _ string) (any, error) {
	removed, err := m.manager.CleanupStale()
	if err != nil {
		return nil, err
	}
	return map[string]any{"removed": removed}, nil
}
