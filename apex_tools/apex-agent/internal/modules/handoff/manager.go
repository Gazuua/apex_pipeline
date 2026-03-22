// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"database/sql"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("handoff")

// Branch represents a registered working branch.
type Branch struct {
	Branch     string
	Workspace  string
	GitBranch  string // git branch --show-current 값
	Status     string
	BacklogIDs []int // junction 테이블에서 조회
	Summary    string
	CreatedAt  string
	UpdatedAt  string
}

// BacklogStatusSetter is the interface handoff needs from backlog.
type BacklogStatusSetter interface {
	SetStatus(id int, status string) error
	Check(id int) (exists bool, status string, err error)
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
	store          *store.Store
	backlogManager BacklogStatusSetter
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store, bm BacklogStatusSetter) *Manager {
	return &Manager{store: s, backlogManager: bm}
}

// NotifyStart registers a new branch and creates a start notification.
// If skipDesign is true, sets status to "implementing" directly.
// gitBranch is stored for fallback lookups by hook system.
// Returns the notification ID.
func (m *Manager) NotifyStart(branch, workspace, summary, gitBranch string, backlogIDs []int, scopes string, skipDesign bool) (int, error) {
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
		`INSERT INTO branches (branch, workspace, git_branch, status, summary, created_at, updated_at)
		 VALUES (?, ?, ?, ?, ?, datetime('now','localtime'), datetime('now','localtime'))`,
		branch, workspace, nullableString(gitBranch), status, summary,
	)
	if err != nil {
		return 0, fmt.Errorf("insert branch: %w", err)
	}

	// junction 테이블에 백로그 연결
	for _, bid := range backlogIDs {
		if bid == 0 {
			continue
		}
		// FIXING 중복 체크
		if m.backlogManager != nil {
			exists, bStatus, checkErr := m.backlogManager.Check(bid)
			if checkErr != nil {
				return 0, fmt.Errorf("check backlog %d: %w", bid, checkErr)
			}
			if !exists {
				return 0, fmt.Errorf("backlog item %d not found", bid)
			}
			if bStatus == "FIXING" {
				return 0, fmt.Errorf("backlog item %d is already FIXING", bid)
			}
		}
		_, err = tx.ExecContext(ctx,
			`INSERT INTO branch_backlogs (branch, backlog_id) VALUES (?, ?)`,
			branch, bid,
		)
		if err != nil {
			return 0, fmt.Errorf("insert branch_backlog %d: %w", bid, err)
		}
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

	// FIXING 상태 전이 (tx 밖 — backlog 모듈이 자체 Exec 사용, 동일 DB이나 인터페이스 제약)
	if m.backlogManager != nil {
		for _, bid := range backlogIDs {
			if bid == 0 {
				continue
			}
			if err := m.backlogManager.SetStatus(bid, "FIXING"); err != nil {
				ml.Warn("backlog FIXING 전이 실패 — 브랜치는 등록됨, 백로그 상태 불일치 가능",
					"backlog_id", bid, "branch", branch, "err", err)
			}
		}
	}

	ml.Audit("branch registered", "branch", branch, "workspace", workspace, "status", status, "notification_id", int(id))
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

	ml.Audit("state transition", "branch", branch, "from", currentStatus, "to", nextStatus, "type", notifyType, "notification_id", int(id))
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
// TODO: "abandoned" 상태 추가 또는 stale timeout 기반 자동 해제 —
//       현재 merge-notified만 제외하므로 영구 미머지 브랜치의 backlog가 점유 상태로 남음.
func (m *Manager) BacklogCheck(backlogID int) (available bool, branch string, err error) {
	row := m.store.QueryRow(
		`SELECT bb.branch FROM branch_backlogs bb
		 JOIN branches b ON b.branch = bb.branch
		 WHERE bb.backlog_id = ? AND b.status != ?`,
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
		`SELECT branch, workspace, COALESCE(git_branch, ''), status, summary, created_at, updated_at
		 FROM branches WHERE branch = ?`,
		branch,
	)

	var b Branch
	var summary sql.NullString
	if err := row.Scan(&b.Branch, &b.Workspace, &b.GitBranch, &b.Status, &summary, &b.CreatedAt, &b.UpdatedAt); err != nil {
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("scan branch: %w", err)
	}
	b.Summary = summary.String

	// junction에서 backlog IDs 조회
	rows, err := m.store.Query(
		`SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch,
	)
	if err != nil {
		return nil, fmt.Errorf("query backlog ids: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var id int
		rows.Scan(&id)
		b.BacklogIDs = append(b.BacklogIDs, id)
	}

	return &b, nil
}

// ResolveBranch finds a branch by workspace ID first, then by git branch name.
// Returns the workspace branch ID (primary key) or empty string if not found.
func (m *Manager) ResolveBranch(workspaceID, gitBranch string) (string, error) {
	// 1차: workspace ID로 조회
	status, err := m.GetStatus(workspaceID)
	if err != nil {
		return "", err
	}
	if status != "" {
		return workspaceID, nil
	}

	// 2차: git branch 이름으로 fallback
	if gitBranch != "" {
		row := m.store.QueryRow(
			`SELECT branch FROM branches WHERE git_branch = ?`, gitBranch,
		)
		var branch string
		if scanErr := row.Scan(&branch); scanErr == nil {
			return branch, nil
		}
	}

	return "", nil
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

// nullableString converts an empty string to nil for SQL NULL storage.
func nullableString(s string) any {
	if s == "" {
		return nil
	}
	return s
}

