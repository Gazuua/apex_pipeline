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
func (m *Manager) TryAcquire(ctx context.Context, channel, branch string, pid int) (bool, error) {
	var acquired bool
	err := m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		// Check if there is already an active entry for this channel.
		hasActive, err := m.hasActiveEntry(tx, channel)
		if err != nil {
			return fmt.Errorf("status check: %w", err)
		}

		if hasActive {
			// Channel is busy — register as waiting (skip duplicate).
			exists, waitErr := m.hasWaitingEntryForBranch(tx, channel, branch)
			if waitErr != nil {
				return fmt.Errorf("check waiting entry: %w", waitErr)
			}
			if !exists {
				if err := m.insertEntryTx(tx, channel, branch, pid, StatusWaiting); err != nil {
					return err
				}
				m.insertHistoryTx(tx, channel, branch, StatusWaiting)
				return nil
			}
			return nil
		}

		// Channel appears free. Check if there are already waiting entries.
		first, err := m.firstWaitingTx(tx, channel)
		if err != nil {
			return fmt.Errorf("first waiting: %w", err)
		}

		// If there's a waiter with a different branch already queued ahead, we must wait.
		if first != nil && first.Branch != branch {
			exists, waitErr := m.hasWaitingEntryForBranch(tx, channel, branch)
			if waitErr != nil {
				return fmt.Errorf("check waiting entry: %w", waitErr)
			}
			if !exists {
				if err := m.insertEntryTx(tx, channel, branch, pid, StatusWaiting); err != nil {
					return err
				}
				m.insertHistoryTx(tx, channel, branch, StatusWaiting)
				return nil
			}
			return nil
		}

		// If there's a waiting entry for this branch, promote it to active.
		if first != nil && first.Branch == branch {
			_, err := tx.Exec(`UPDATE queue SET status=? WHERE id=?`, StatusActive, first.ID)
			if err != nil {
				return fmt.Errorf("promote waiting to active: %w", err)
			}
			m.insertHistoryTx(tx, channel, first.Branch, StatusActive)
			acquired = true
			return nil
		}

		// No waiters at all — insert directly as active.
		if err := m.insertEntryTx(tx, channel, branch, pid, StatusActive); err != nil {
			return err
		}
		m.insertHistoryTx(tx, channel, branch, StatusActive)
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

// Backoff constants for Acquire polling.
const (
	backoffInitial     = 100 * time.Millisecond
	backoffMax         = 2 * time.Second
	backoffFactor      = 2
	cleanupStaleMinGap = 5 * time.Second
)

// Acquire acquires the lock for channel, blocking until it succeeds.
// It inserts a waiting entry then polls until it becomes first in queue
// and no active entry exists. Promote uses CAS pattern to prevent TOCTOU races.
// Polling uses exponential backoff: 100ms → 200ms → 400ms → ... → 2s (cap).
func (m *Manager) Acquire(ctx context.Context, channel, branch string, pid int) error {
	// Register as waiting first (skip if already queued).
	// Wrapped in transaction to prevent duplicate WAITING entries from concurrent calls.
	if err := m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		exists, waitErr := m.hasWaitingEntryForBranch(tx, channel, branch)
		if waitErr != nil {
			return fmt.Errorf("check waiting: %w", waitErr)
		}
		if !exists {
			if err := m.insertEntryTx(tx, channel, branch, pid, StatusWaiting); err != nil {
				return err
			}
			m.insertHistoryTx(tx, channel, branch, StatusWaiting)
			return nil
		}
		return nil
	}); err != nil {
		return fmt.Errorf("queue.Acquire: %w", err)
	}

	delay := backoffInitial
	lastCleanup := time.Time{} // zero → 첫 반복에서 즉시 실행
	for {
		select {
		case <-ctx.Done():
			// 취소/타임아웃 시 대기 엔트리만 정리 (ACTIVE는 이미 promote된 상태이므로 보존)
			if _, err := m.store.Exec(
				`DELETE FROM queue WHERE channel=? AND branch=? AND status=?`,
				channel, branch, StatusWaiting,
			); err != nil {
				ml.Warn("failed to cleanup queue entry on cancel", "channel", channel, "branch", branch, "err", err)
			}
			return fmt.Errorf("queue.Acquire: %w", ctx.Err())
		default:
		}

		// Clean up dead processes (throttled — at most once per cleanupStaleMinGap).
		if time.Since(lastCleanup) >= cleanupStaleMinGap {
			if _, err := m.CleanupStale(); err != nil {
				return fmt.Errorf("queue.Acquire: cleanup: %w", err)
			}
			lastCleanup = time.Now()
		}

		// Atomic check-and-promote: 트랜잭션으로 active 부재 + first-in-queue 확인 + promote
		// tryPromote uses Background context intentionally — it's a short transaction
		// and cancel cleanup is handled by the ctx.Done() path above.
		promoted, err := m.tryPromote(context.Background(), channel, branch)
		if err != nil {
			return fmt.Errorf("queue.Acquire: promote: %w", err)
		}
		if promoted {
			ml.Audit("lock acquired", "channel", channel, "branch", branch, "pid", pid, "method", "blocking")
			return nil
		}

		time.Sleep(delay)
		delay *= backoffFactor
		if delay > backoffMax {
			delay = backoffMax
		}
	}
}

// tryPromote atomically checks if channel is free and branch is first-in-queue,
// then promotes the waiting entry to active. Returns true if promoted.
func (m *Manager) tryPromote(ctx context.Context, channel, branch string) (bool, error) {
	var promoted bool
	err := m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		hasActive, err := m.hasActiveEntry(tx, channel)
		if err != nil {
			return err
		}
		if hasActive {
			return nil
		}

		first, err := m.firstWaitingTx(tx, channel)
		if err != nil {
			return err
		}
		if first == nil || first.Branch != branch {
			return nil
		}

		// CAS: only promote if still WAITING (prevents double-promote)
		res, err := tx.Exec(
			`UPDATE queue SET status=? WHERE id=? AND status=?`,
			StatusActive, first.ID, StatusWaiting,
		)
		if err != nil {
			return err
		}
		n, _ := res.RowsAffected()
		promoted = n > 0
		if promoted {
			m.insertHistoryTx(tx, channel, branch, StatusActive)
		}
		return nil
	})
	return promoted, err
}

// UpdatePID updates the PID of the active entry for a channel.
// Used to transfer lock ownership from parent (CLI) to child (build process).
func (m *Manager) UpdatePID(channel, branch string, newPID int) error {
	res, err := m.store.Exec(
		`UPDATE queue SET pid=? WHERE channel=? AND branch=? AND status=?`,
		newPID, channel, branch, StatusActive,
	)
	if err != nil {
		return fmt.Errorf("queue.UpdatePID: %w", err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("queue.UpdatePID: no active entry for channel %q", channel)
	}
	ml.Audit("lock pid updated", "channel", channel, "new_pid", newPID)
	return nil
}

// Release marks the active entry for channel as done and records finish time.
// Idempotent: returns nil if no active entry exists (already released or cleaned up).
func (m *Manager) Release(channel string) error {
	var released bool
	var branch string
	err := m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		// Read the active branch (for history).
		_ = tx.QueryRow(
			`SELECT branch FROM queue WHERE channel=? AND status=?`,
			channel, StatusActive,
		).Scan(&branch)

		res, err := tx.Exec(
			`UPDATE queue SET status=?, finished_at=datetime('now','localtime') WHERE channel=? AND status=?`,
			StatusDone, channel, StatusActive,
		)
		if err != nil {
			return fmt.Errorf("queue.Release: %w", err)
		}
		n, _ := res.RowsAffected()
		if n == 0 {
			ml.Warn("release: no active entry found (already released or stale-cleaned)", "channel", channel)
		} else {
			released = true
			m.insertHistoryTx(tx, channel, branch, StatusDone)
		}
		return nil
	})
	if err != nil {
		return err
	}
	if released {
		ml.Audit("lock released", "channel", channel)
	}
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
	defer rows.Close()

	type row struct {
		id  int
		pid int
	}
	var stale []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.id, &r.pid); err != nil {
			return 0, fmt.Errorf("queue.CleanupStale: scan: %w", err)
		}
		if !platform.IsProcessAlive(r.pid) {
			stale = append(stale, r)
		}
	}
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

// ── Dashboard queries ─────────────────────────────────────────────────────────

// DashboardEntry is a view for dashboard queue display.
type DashboardEntry struct {
	Channel    string
	Branch     string
	Status     string
	CreatedAt  string
	FinishedAt string
	DurationSec int // computed: finished_at - created_at in seconds
}

// DashboardQueueAll returns all queue entries for dashboard display.
func (m *Manager) DashboardQueueAll() ([]DashboardEntry, error) {
	rows, err := m.store.Query(`
		SELECT channel, branch, status, created_at, COALESCE(finished_at,''),
		       CASE WHEN finished_at IS NOT NULL
		            THEN CAST((julianday(finished_at) - julianday(created_at)) * 86400 AS INTEGER)
		            ELSE 0 END as duration_sec
		FROM queue
		ORDER BY channel, created_at DESC`)
	if err != nil {
		return nil, fmt.Errorf("DashboardQueueAll: %w", err)
	}
	defer rows.Close()

	var entries []DashboardEntry
	for rows.Next() {
		var q DashboardEntry
		if err := rows.Scan(&q.Channel, &q.Branch, &q.Status, &q.CreatedAt, &q.FinishedAt, &q.DurationSec); err != nil {
			return nil, fmt.Errorf("DashboardQueueAll scan: %w", err)
		}
		entries = append(entries, q)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("DashboardQueueAll rows: %w", err)
	}
	return entries, nil
}

// DashboardLockStatus returns whether a channel has an active lock.
func (m *Manager) DashboardLockStatus(channel string) (bool, error) {
	var count int
	if err := m.store.QueryRow(
		`SELECT COUNT(*) FROM queue WHERE channel=? AND status='ACTIVE'`, channel,
	).Scan(&count); err != nil {
		return false, fmt.Errorf("DashboardLockStatus %s: %w", channel, err)
	}
	return count > 0, nil
}

// insertEntry inserts a new queue entry with the given status (non-transactional).
func (m *Manager) insertEntry(channel, branch string, pid int, status string) error {
	return m.insertEntryTx(m.store, channel, branch, pid, status)
}

// insertEntryTx inserts a new queue entry using the given store (transaction-safe).
func (m *Manager) insertEntryTx(s store.Querier, channel, branch string, pid int, status string) error {
	_, err := s.Exec(
		`INSERT INTO queue (channel, branch, pid, status) VALUES (?, ?, ?, ?)`,
		channel, branch, pid, status,
	)
	return err
}

// hasActiveEntry checks if there is an active entry for the channel (transaction-safe).
func (m *Manager) hasActiveEntry(s store.Querier, channel string) (bool, error) {
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

// hasWaitingEntryForBranch checks if a waiting entry already exists for channel+branch (transaction-safe).
func (m *Manager) hasWaitingEntryForBranch(s store.Querier, channel, branch string) (bool, error) {
	row := s.QueryRow(
		`SELECT COUNT(*) FROM queue WHERE channel=? AND branch=? AND status=?`,
		channel, branch, StatusWaiting,
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
func (m *Manager) firstWaitingTx(s store.Querier, channel string) (*QueueEntry, error) {
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

// ── History event log ─────────────────────────────────────────────────────────

// HistoryEntry represents a row in the queue_history table.
type HistoryEntry struct {
	ID        int
	Channel   string
	Branch    string
	Status    string
	Timestamp string
}

// insertHistory records a state-transition event in queue_history.
func (m *Manager) insertHistory(channel, branch, status string) {
	_, err := m.store.Exec(
		`INSERT INTO queue_history (channel, branch, status) VALUES (?, ?, ?)`,
		channel, branch, status,
	)
	if err != nil {
		ml.Warn("failed to insert queue history", "channel", channel, "branch", branch, "status", status, "err", err)
	}
}

// insertHistoryTx records a state-transition event using a transaction store.
func (m *Manager) insertHistoryTx(s store.Querier, channel, branch, status string) {
	_, err := s.Exec(
		`INSERT INTO queue_history (channel, branch, status) VALUES (?, ?, ?)`,
		channel, branch, status,
	)
	if err != nil {
		ml.Warn("failed to insert queue history (tx)", "channel", channel, "branch", branch, "status", status, "err", err)
	}
}

// DashboardHistory returns history events for a channel, newest first.
// Supports pagination (offset+limit) and optional time range filter (from/to as ISO datetime).
func (m *Manager) DashboardHistory(channel string, offset, limit int, from, to string) ([]HistoryEntry, error) {
	query := `SELECT id, channel, branch, status, timestamp FROM queue_history WHERE channel = ?`
	args := []any{channel}

	if from != "" {
		query += ` AND timestamp >= ?`
		args = append(args, from)
	}
	if to != "" {
		query += ` AND timestamp <= ?`
		args = append(args, to)
	}

	query += ` ORDER BY id DESC LIMIT ? OFFSET ?`
	args = append(args, limit, offset)

	rows, err := m.store.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("DashboardHistory: %w", err)
	}
	defer rows.Close()

	var entries []HistoryEntry
	for rows.Next() {
		var e HistoryEntry
		if err := rows.Scan(&e.ID, &e.Channel, &e.Branch, &e.Status, &e.Timestamp); err != nil {
			return nil, fmt.Errorf("DashboardHistory scan: %w", err)
		}
		entries = append(entries, e)
	}
	return entries, rows.Err()
}
