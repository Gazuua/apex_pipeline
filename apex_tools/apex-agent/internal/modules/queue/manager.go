// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package queue

import (
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("queue")

// QueueEntry represents a row in the queue table.
type QueueEntry struct {
	ID        int
	Channel   string
	Branch    string
	PID       int
	Status    string
	CreatedAt string
}

// Manager handles FIFO queue and channel lock operations.
type Manager struct {
	store *store.Store
}

// NewManager creates a Manager backed by the given store.
func NewManager(s *store.Store) *Manager {
	return &Manager{store: s}
}

// TryAcquire attempts to acquire the lock for channel without blocking.
// If the channel is free and no earlier waiters exist, the entry is created
// with status='active' and true is returned.
// If the channel is busy or earlier waiters exist, an entry with status='waiting'
// is inserted and false is returned.
func (m *Manager) TryAcquire(channel, branch string, pid int) (bool, error) {
	// Check if there is already an active entry for this channel.
	active, _, err := m.Status(channel)
	if err != nil {
		return false, fmt.Errorf("queue.TryAcquire: status check: %w", err)
	}

	if active != nil {
		// Channel is busy — register as waiting.
		if err := m.insertEntry(channel, branch, pid, "waiting"); err != nil {
			return false, fmt.Errorf("queue.TryAcquire: insert waiting: %w", err)
		}
		return false, nil
	}

	// Channel appears free. Check if there are already waiting entries with lower IDs
	// (other branches queued ahead).
	firstWaiting, err := m.firstWaiting(channel)
	if err != nil {
		return false, fmt.Errorf("queue.TryAcquire: first waiting: %w", err)
	}

	// If there's a waiter with a different branch already queued ahead, we must wait.
	if firstWaiting != nil && firstWaiting.Branch != branch {
		if err := m.insertEntry(channel, branch, pid, "waiting"); err != nil {
			return false, fmt.Errorf("queue.TryAcquire: insert waiting (queue not empty): %w", err)
		}
		return false, nil
	}

	// If there's a waiting entry for this branch (already registered), promote it to active.
	if firstWaiting != nil && firstWaiting.Branch == branch {
		_, err := m.store.Exec(
			`UPDATE queue SET status='active' WHERE id=?`,
			firstWaiting.ID,
		)
		if err != nil {
			return false, fmt.Errorf("queue.TryAcquire: promote waiting to active: %w", err)
		}
		return true, nil
	}

	// No waiters at all — insert directly as active.
	if err := m.insertEntry(channel, branch, pid, "active"); err != nil {
		return false, fmt.Errorf("queue.TryAcquire: insert active: %w", err)
	}
	ml.Audit("lock acquired", "channel", channel, "branch", branch, "pid", pid, "method", "try")
	return true, nil
}

// Acquire acquires the lock for channel, blocking until it succeeds.
// It inserts a waiting entry then polls until it becomes first in queue
// and no active entry exists.
func (m *Manager) Acquire(channel, branch string, pid int) error {
	// Register as waiting first.
	if err := m.insertEntry(channel, branch, pid, "waiting"); err != nil {
		return fmt.Errorf("queue.Acquire: insert: %w", err)
	}

	for {
		// Clean up dead processes.
		if _, err := m.CleanupStale(); err != nil {
			return fmt.Errorf("queue.Acquire: cleanup: %w", err)
		}

		// Check if channel is free and we are first.
		active, _, err := m.Status(channel)
		if err != nil {
			return fmt.Errorf("queue.Acquire: status: %w", err)
		}
		if active != nil {
			time.Sleep(500 * time.Millisecond)
			continue
		}

		first, err := m.firstWaiting(channel)
		if err != nil {
			return fmt.Errorf("queue.Acquire: first waiting: %w", err)
		}
		if first == nil || first.Branch != branch {
			time.Sleep(500 * time.Millisecond)
			continue
		}

		// Promote our waiting entry to active.
		_, err = m.store.Exec(
			`UPDATE queue SET status='active' WHERE id=?`,
			first.ID,
		)
		if err != nil {
			return fmt.Errorf("queue.Acquire: promote: %w", err)
		}
		ml.Audit("lock acquired", "channel", channel, "branch", branch, "pid", pid, "method", "blocking")
		return nil
	}
}

// Release marks the active entry for channel as done.
func (m *Manager) Release(channel string) error {
	_, err := m.store.Exec(
		`UPDATE queue SET status='done' WHERE channel=? AND status='active'`,
		channel,
	)
	if err != nil {
		return fmt.Errorf("queue.Release: %w", err)
	}
	ml.Audit("lock released", "channel", channel)
	return nil
}

// Status returns the active entry (or nil) and all waiting entries for channel.
func (m *Manager) Status(channel string) (*QueueEntry, []QueueEntry, error) {
	rows, err := m.store.Query(
		`SELECT id, channel, branch, pid, status, created_at
		 FROM queue
		 WHERE channel=? AND status IN ('active','waiting')
		 ORDER BY id ASC`,
		channel,
	)
	if err != nil {
		return nil, nil, fmt.Errorf("queue.Status: query: %w", err)
	}
	defer rows.Close()

	var active *QueueEntry
	var waiting []QueueEntry

	for rows.Next() {
		var e QueueEntry
		if err := rows.Scan(&e.ID, &e.Channel, &e.Branch, &e.PID, &e.Status, &e.CreatedAt); err != nil {
			return nil, nil, fmt.Errorf("queue.Status: scan: %w", err)
		}
		switch e.Status {
		case "active":
			ec := e
			active = &ec
		case "waiting":
			waiting = append(waiting, e)
		}
	}
	if err := rows.Err(); err != nil {
		return nil, nil, fmt.Errorf("queue.Status: rows: %w", err)
	}
	return active, waiting, nil
}

// CleanupStale removes queue entries whose owning process is no longer alive.
// Returns the number of removed entries.
func (m *Manager) CleanupStale() (int, error) {
	rows, err := m.store.Query(
		`SELECT id, pid FROM queue WHERE status IN ('active','waiting')`,
	)
	if err != nil {
		return 0, fmt.Errorf("queue.CleanupStale: query: %w", err)
	}

	type row struct {
		id  int
		pid int
	}
	var stale []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.pid); err != nil {
			rows.Close()
			return 0, fmt.Errorf("queue.CleanupStale: scan: %w", err)
		}
		if !platform.IsProcessAlive(r.pid) {
			stale = append(stale, r)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("queue.CleanupStale: rows: %w", err)
	}

	count := 0
	for _, r := range stale {
		if _, err := m.store.Exec(`DELETE FROM queue WHERE id=?`, r.id); err != nil {
			return count, fmt.Errorf("queue.CleanupStale: delete id=%d: %w", r.id, err)
		}
		count++
	}
	return count, nil
}

// insertEntry inserts a new queue entry with the given status.
func (m *Manager) insertEntry(channel, branch string, pid int, status string) error {
	_, err := m.store.Exec(
		`INSERT INTO queue (channel, branch, pid, status) VALUES (?, ?, ?, ?)`,
		channel, branch, pid, status,
	)
	if err != nil {
		return err
	}
	return nil
}

// firstWaiting returns the waiting entry with the lowest ID for channel, or nil.
func (m *Manager) firstWaiting(channel string) (*QueueEntry, error) {
	row := m.store.QueryRow(
		`SELECT id, channel, branch, pid, status, created_at
		 FROM queue
		 WHERE channel=? AND status='waiting'
		 ORDER BY id ASC
		 LIMIT 1`,
		channel,
	)
	var e QueueEntry
	err := row.Scan(&e.ID, &e.Channel, &e.Branch, &e.PID, &e.Status, &e.CreatedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &e, nil
}
