// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"context"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// DBUpdater is called by the watchdog to update session state in the DB.
// Avoids direct store dependency — caller provides the callback.
type DBUpdater func(workspaceID string, status string, pid int, sessionID string, logPath string)

// Watchdog periodically checks if managed processes are still alive.
// Dead sessions are removed from the Manager and their DB state is reset to STOP.
type Watchdog struct {
	mgr      *Manager
	interval time.Duration
	onUpdate DBUpdater
}

// NewWatchdog creates a watchdog that checks at the given interval.
func NewWatchdog(mgr *Manager, interval time.Duration, onUpdate DBUpdater) *Watchdog {
	return &Watchdog{
		mgr:      mgr,
		interval: interval,
		onUpdate: onUpdate,
	}
}

// Run starts the watchdog loop. Blocks until ctx is canceled.
func (w *Watchdog) Run(ctx context.Context) {
	ticker := time.NewTicker(w.interval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			w.check()
		}
	}
}

func (w *Watchdog) check() {
	sessions := w.mgr.List()
	for _, s := range sessions {
		if s.PID <= 0 {
			continue
		}
		if !platform.IsProcessAlive(s.PID) {
			ml.Info("session process dead, cleaning up", "workspace", s.WorkspaceID, "pid", s.PID)
			w.mgr.Remove(s.WorkspaceID)
			if w.onUpdate != nil {
				w.onUpdate(s.WorkspaceID, StatusStop, 0, "", "")
			}
		}
	}
}
