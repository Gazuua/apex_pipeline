// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package queue

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("queue")

// Queue status constants.
const (
	StatusWaiting = "WAITING"
	StatusActive  = "ACTIVE"
	StatusDone    = "DONE"
)

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
// Entire operation is wrapped in a transaction to prevent TOCTOU races.
func (m *Manager) TryAcquire(channel, branch string, pid int) (bool, error) {
	var acquired bool
	err := m.store.RunInTx(context.Background(), func(txs *store.Store) error {
		// Check if there is already an active entry for this channel.
		hasActive, err := m.hasActiveEntry(txs, channel)
		if err != nil {
			return fmt.Errorf("status check: %w", err)
		}

		if hasActive {
			// Channel is busy — register as waiting (skip duplicate).
			return m.insertEntryTx(txs, channel, branch, pid, StatusWaiting)
		}

		// Channel appears free. Check if there are already waiting entries.
		first, err := m.firstWaitingTx(txs, channel)
		if err != nil {
			return fmt.Errorf("first waiting: %w", err)
		}

		// If there's a waiter with a different branch already queued ahead, we must wait.
		if first != nil && first.Branch != branch {
			return m.insertEntryTx(txs, channel, branch, pid, StatusWaiting)
		}

		// If there's a waiting entry for this branch, promote it to active.
		if first != nil && first.Branch == branch {
			_, err := txs.Exec(`UPDATE queue SET status=? WHERE id=?`, StatusActive, first.ID)
			if err != nil {
				return fmt.Errorf("promote waiting to active: %w", err)
			}
			acquired = true
			return nil
		}

		// No waiters at all — insert directly as active.
		if err := m.insertEntryTx(txs, channel, branch, pid, StatusActive); err != nil {
			return err
		}
		acquired = true
		return nil
	})
	if err != nil {
		return false, fmt.Errorf("queue.TryAcquire: %w", err)
	}
	if acquired {
		ml.Audit("lock acquired", "channel", channel, "branch", branch, "pid", pid, "method", "try")
	}
	return acquired, nil
}

// Acquire acquires the lock for channel, blocking until it succeeds.
// It inserts a waiting entry then polls until it becomes first in queue
// and no active entry exists. Promote uses CAS pattern to prevent TOCTOU races.
func (m *Manager) Acquire(ctx context.Context, channel, branch string, pid int) error {
	// Register as waiting first.
	if err := m.insertEntry(channel, branch, pid, StatusWaiting); err != nil {
		return fmt.Errorf("queue.Acquire: insert: %w", err)
	}

	for {
		select {
		case <-ctx.Done():
			// 취소/타임아웃 시 대기+활성 엔트리 모두 정리
			if _, err := m.store.Exec(
				`DELETE FROM queue WHERE channel=? AND branch=? AND status IN (?, ?)`,
				channel, branch, StatusWaiting, StatusActive,
			); err != nil {
				ml.Warn("failed to cleanup queue entry on cancel", "channel", channel, "branch", branch, "err", err)
			}
			return fmt.Errorf("queue.Acquire: %w", ctx.Err())
		default:
		}

		// Clean up dead processes.
		if _, err := m.CleanupStale(); err != nil {
			return fmt.Errorf("queue.Acquire: cleanup: %w", err)
		}

		// Atomic check-and-promote: 트랜잭션으로 active 부재 + first-in-queue 확인 + promote
		promoted, err := m.tryPromote(channel, branch)
		if err != nil {
			return fmt.Errorf("queue.Acquire: promote: %w", err)
		}
		if promoted {
			ml.Audit("lock acquired", "channel", channel, "branch", branch, "pid", pid, "method", "blocking")
			return nil
		}

		time.Sleep(500 * time.Millisecond)
	}
}

// tryPromote atomically checks if channel is free and branch is first-in-queue,
// then promotes the waiting entry to active. Returns true if promoted.
func (m *Manager) tryPromote(channel, branch string) (bool, error) {
	var promoted bool
	err := m.store.RunInTx(context.Background(), func(txs *store.Store) error {
		hasActive, err := m.hasActiveEntry(txs, channel)
		if err != nil {
			return err
		}
		if hasActive {
			return nil
		}

		first, err := m.firstWaitingTx(txs, channel)
		if err != nil {
			return err
		}
		if first == nil || first.Branch != branch {
			return nil
		}

		// CAS: only promote if still WAITING (prevents double-promote)
		res, err := txs.Exec(
			`UPDATE queue SET status=? WHERE id=? AND status=?`,
			StatusActive, first.ID, StatusWaiting,
		)
		if err != nil {
			return err
		}
		n, _ := res.RowsAffected()
		promoted = n > 0
		return nil
	})
	return promoted, err
}

// Release marks the active entry for channel as done.
func (m *Manager) Release(channel string) error {
	_, err := m.store.Exec(
		`UPDATE queue SET status=? WHERE channel=? AND status=?`,
		StatusDone, channel, StatusActive,
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
		 WHERE channel=? AND status IN (?, ?)
		 ORDER BY id ASC`,
		channel, StatusActive, StatusWaiting,
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
		case StatusActive:
			ec := e
			active = &ec
		case StatusWaiting:
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
		`SELECT id, pid FROM queue WHERE status IN (?, ?)`,
		StatusActive, StatusWaiting,
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

// insertEntry inserts a new queue entry with the given status (non-transactional).
func (m *Manager) insertEntry(channel, branch string, pid int, status string) error {
	return m.insertEntryTx(m.store, channel, branch, pid, status)
}

// insertEntryTx inserts a new queue entry using the given store (transaction-safe).
func (m *Manager) insertEntryTx(s *store.Store, channel, branch string, pid int, status string) error {
	_, err := s.Exec(
		`INSERT INTO queue (channel, branch, pid, status) VALUES (?, ?, ?, ?)`,
		channel, branch, pid, status,
	)
	return err
}

// hasActiveEntry checks if there is an active entry for the channel (transaction-safe).
func (m *Manager) hasActiveEntry(s *store.Store, channel string) (bool, error) {
	row := s.QueryRow(
		`SELECT COUNT(*) FROM queue WHERE channel=? AND status=?`,
		channel, StatusActive,
	)
	var count int
	if err := row.Scan(&count); err != nil {
		return false, err
	}
	return count > 0, nil
}

// firstWaiting returns the waiting entry with the lowest ID for channel, or nil (non-transactional).
func (m *Manager) firstWaiting(channel string) (*QueueEntry, error) {
	return m.firstWaitingTx(m.store, channel)
}

// firstWaitingTx returns the waiting entry with the lowest ID for channel (transaction-safe).
func (m *Manager) firstWaitingTx(s *store.Store, channel string) (*QueueEntry, error) {
	row := s.QueryRow(
		`SELECT id, channel, branch, pid, status, created_at
		 FROM queue
		 WHERE channel=? AND status=?
		 ORDER BY id ASC
		 LIMIT 1`,
		channel, StatusWaiting,
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
