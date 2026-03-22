// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"database/sql"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Branch represents a registered working branch.
type Branch struct {
	Branch    string
	Workspace string
	Status    string
	BacklogID int
	Summary   string
	CreatedAt string
	UpdatedAt string
}

// Notification represents a handoff notification event.
type Notification struct {
	ID        int
	Branch    string
	Workspace string
	Type      string
	Summary   string
	Payload   string
	CreatedAt string
}

// Ack represents an acknowledgement of a notification by a branch.
type Ack struct {
	NotificationID int
	Branch         string
	Action         string
	AckedAt        string
}

// Manager provides handoff business logic.
type Manager struct {
	store *store.Store
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store) *Manager {
	return &Manager{store: s}
}

// NotifyStart registers a new branch and creates a start notification.
// If skipDesign is true, sets status to "implementing" directly.
// Returns the notification ID.
func (m *Manager) NotifyStart(branch, workspace, summary string, backlogID int, scopes string, skipDesign bool) (int, error) {
	status := StatusStarted
	if skipDesign {
		status = StatusImplementing
	}

	ctx := context.Background()
	tx, err := m.store.BeginTx(ctx)
	if err != nil {
		return 0, fmt.Errorf("begin tx: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	_, err = tx.ExecContext(ctx,
		`INSERT INTO branches (branch, workspace, status, backlog_id, summary, created_at, updated_at)
		 VALUES (?, ?, ?, ?, ?, datetime('now','localtime'), datetime('now','localtime'))`,
		branch, workspace, status, nullableInt(backlogID), summary,
	)
	if err != nil {
		return 0, fmt.Errorf("insert branch: %w", err)
	}

	res, err := tx.ExecContext(ctx,
		`INSERT INTO notifications (branch, workspace, type, summary, created_at)
		 VALUES (?, ?, 'start', ?, datetime('now','localtime'))`,
		branch, workspace, summary,
	)
	if err != nil {
		return 0, fmt.Errorf("insert notification: %w", err)
	}

	id, err := res.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("last insert id: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("commit: %w", err)
	}

	return int(id), nil
}

// NotifyTransition applies a state transition (design/plan/merge) and creates a notification.
// Uses NextStatus() to validate the transition.
// Returns the notification ID.
func (m *Manager) NotifyTransition(branch, workspace, notifyType, summary string) (int, error) {
	currentStatus, err := m.GetStatus(branch)
	if err != nil {
		return 0, fmt.Errorf("get status: %w", err)
	}
	if currentStatus == "" {
		return 0, fmt.Errorf("branch %q is not registered", branch)
	}

	nextStatus, err := NextStatus(currentStatus, notifyType)
	if err != nil {
		return 0, err
	}

	ctx := context.Background()
	tx, err := m.store.BeginTx(ctx)
	if err != nil {
		return 0, fmt.Errorf("begin tx: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	_, err = tx.ExecContext(ctx,
		`UPDATE branches SET status = ?, updated_at = datetime('now','localtime') WHERE branch = ?`,
		nextStatus, branch,
	)
	if err != nil {
		return 0, fmt.Errorf("update branch status: %w", err)
	}

	res, err := tx.ExecContext(ctx,
		`INSERT INTO notifications (branch, workspace, type, summary, created_at)
		 VALUES (?, ?, ?, ?, datetime('now','localtime'))`,
		branch, workspace, notifyType, summary,
	)
	if err != nil {
		return 0, fmt.Errorf("insert notification: %w", err)
	}

	id, err := res.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("last insert id: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("commit: %w", err)
	}

	return int(id), nil
}

// CheckNotifications returns unacked notifications for a branch.
// Excludes notifications sent by the branch itself.
func (m *Manager) CheckNotifications(branch string) ([]Notification, error) {
	rows, err := m.store.Query(
		`SELECT n.id, n.branch, n.workspace, n.type, n.summary, n.payload, n.created_at
		 FROM notifications n
		 WHERE n.branch != ?
		 AND n.id NOT IN (
		     SELECT notification_id FROM notification_acks WHERE branch = ?
		 )
		 ORDER BY n.id`,
		branch, branch,
	)
	if err != nil {
		return nil, fmt.Errorf("query notifications: %w", err)
	}
	defer rows.Close()

	var notifs []Notification
	for rows.Next() {
		var n Notification
		var summary, payload sql.NullString
		if err := rows.Scan(&n.ID, &n.Branch, &n.Workspace, &n.Type, &summary, &payload, &n.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan notification: %w", err)
		}
		n.Summary = summary.String
		n.Payload = payload.String
		notifs = append(notifs, n)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("rows error: %w", err)
	}

	return notifs, nil
}

// Ack acknowledges a notification for a branch.
func (m *Manager) Ack(notificationID int, branch, action string) error {
	_, err := m.store.Exec(
		`INSERT OR REPLACE INTO notification_acks (notification_id, branch, action, acked_at)
		 VALUES (?, ?, ?, datetime('now','localtime'))`,
		notificationID, branch, action,
	)
	if err != nil {
		return fmt.Errorf("insert ack: %w", err)
	}
	return nil
}

// BacklogCheck checks if a backlog item is being worked on by any active branch.
// Returns available=true and empty branch if no active branch has this backlog ID.
func (m *Manager) BacklogCheck(backlogID int) (available bool, branch string, err error) {
	row := m.store.QueryRow(
		`SELECT branch FROM branches WHERE backlog_id = ? AND status != ?`,
		backlogID, StatusMergeNotified,
	)
	var b string
	if scanErr := row.Scan(&b); scanErr != nil {
		if scanErr == sql.ErrNoRows {
			return true, "", nil
		}
		return false, "", fmt.Errorf("query backlog check: %w", scanErr)
	}
	return false, b, nil
}

// GetBranch retrieves a branch's current state.
// Returns nil, nil if not found.
func (m *Manager) GetBranch(branch string) (*Branch, error) {
	row := m.store.QueryRow(
		`SELECT branch, workspace, status, backlog_id, summary, created_at, updated_at
		 FROM branches WHERE branch = ?`,
		branch,
	)

	var b Branch
	var backlogID sql.NullInt64
	var summary sql.NullString
	if err := row.Scan(&b.Branch, &b.Workspace, &b.Status, &backlogID, &summary, &b.CreatedAt, &b.UpdatedAt); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("scan branch: %w", err)
	}
	b.BacklogID = int(backlogID.Int64)
	b.Summary = summary.String
	return &b, nil
}

// GetStatus returns the current status of a branch (empty string if not registered).
func (m *Manager) GetStatus(branch string) (string, error) {
	row := m.store.QueryRow(
		`SELECT status FROM branches WHERE branch = ?`,
		branch,
	)
	var status string
	if err := row.Scan(&status); err != nil {
		if err == sql.ErrNoRows {
			return "", nil
		}
		return "", fmt.Errorf("scan status: %w", err)
	}
	return status, nil
}

// nullableInt converts 0 to nil (NULL) for optional integer fields.
func nullableInt(v int) interface{} {
	if v == 0 {
		return nil
	}
	return v
}
