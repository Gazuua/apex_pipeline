// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"context"
	"fmt"
	"os/exec"
	"strings"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("workspace")

// LocalBranch represents a workspace directory entry in local_branches table.
type LocalBranch struct {
	WorkspaceID   string `json:"workspace_id"`
	Directory     string `json:"directory"`
	GitBranch     string `json:"git_branch"`
	GitStatus     string `json:"git_status"`
	SessionID     string `json:"session_id,omitempty"`
	SessionPID    int    `json:"session_pid"`
	SessionStatus string `json:"session_status"`
	SessionLog    string `json:"session_log,omitempty"`
	LastScanned   string `json:"last_scanned,omitempty"`
	CreatedAt     string `json:"created_at"`
}

// SessionUpdate holds fields for updating session state of a branch.
type SessionUpdate struct {
	SessionID     string
	SessionPID    int
	SessionStatus string
	SessionLog    string
}

// ScanResult reports how many branches were added/removed during a scan.
type ScanResult struct {
	Added   int `json:"added"`
	Removed int `json:"removed"`
}

// Manager handles workspace CRUD operations.
type Manager struct {
	store *store.Store
	q     store.Querier
	cfg   *config.WorkspaceConfig
}

// NewManager creates a workspace Manager.
func NewManager(s *store.Store, cfg *config.WorkspaceConfig) *Manager {
	return &Manager{store: s, q: s, cfg: cfg}
}

// Upsert inserts or updates a local branch entry.
func (m *Manager) Upsert(ctx context.Context, b *LocalBranch) error {
	_, err := m.q.Exec(ctx, `
		INSERT INTO local_branches (workspace_id, directory, git_branch, git_status, last_scanned)
		VALUES (?, ?, ?, ?, datetime('now','localtime'))
		ON CONFLICT(workspace_id) DO UPDATE SET
			directory    = excluded.directory,
			git_branch   = excluded.git_branch,
			git_status   = excluded.git_status,
			last_scanned = datetime('now','localtime')
	`, b.WorkspaceID, b.Directory, b.GitBranch, b.GitStatus)
	return err
}

// Get retrieves a single branch by workspace ID.
func (m *Manager) Get(ctx context.Context, workspaceID string) (*LocalBranch, error) {
	row := m.q.QueryRow(ctx, `
		SELECT workspace_id, directory,
			COALESCE(git_branch,''), COALESCE(git_status,'UNKNOWN'),
			COALESCE(session_id,''), COALESCE(session_pid,0), COALESCE(session_status,'STOP'),
			COALESCE(session_log,''), COALESCE(last_scanned,''), COALESCE(created_at,'')
		FROM local_branches WHERE workspace_id = ?
	`, workspaceID)
	var b LocalBranch
	err := row.Scan(&b.WorkspaceID, &b.Directory, &b.GitBranch, &b.GitStatus,
		&b.SessionID, &b.SessionPID, &b.SessionStatus, &b.SessionLog, &b.LastScanned, &b.CreatedAt)
	if err != nil {
		return nil, fmt.Errorf("get workspace %s: %w", workspaceID, err)
	}
	return &b, nil
}

// List returns all local branches ordered by workspace ID.
func (m *Manager) List(ctx context.Context) ([]LocalBranch, error) {
	rows, err := m.q.Query(ctx, `
		SELECT workspace_id, directory,
			COALESCE(git_branch,''), COALESCE(git_status,'UNKNOWN'),
			COALESCE(session_id,''), COALESCE(session_pid,0), COALESCE(session_status,'STOP'),
			COALESCE(session_log,''), COALESCE(last_scanned,''), COALESCE(created_at,'')
		FROM local_branches ORDER BY workspace_id
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []LocalBranch
	for rows.Next() {
		var b LocalBranch
		if err := rows.Scan(&b.WorkspaceID, &b.Directory, &b.GitBranch, &b.GitStatus,
			&b.SessionID, &b.SessionPID, &b.SessionStatus, &b.SessionLog, &b.LastScanned, &b.CreatedAt); err != nil {
			return nil, err
		}
		result = append(result, b)
	}
	return result, rows.Err()
}

// Delete removes a branch entry by workspace ID.
func (m *Manager) Delete(ctx context.Context, workspaceID string) error {
	_, err := m.q.Exec(ctx, `DELETE FROM local_branches WHERE workspace_id = ?`, workspaceID)
	return err
}

// UpdateSession updates session-related fields for a branch.
func (m *Manager) UpdateSession(ctx context.Context, workspaceID string, u SessionUpdate) error {
	_, err := m.q.Exec(ctx, `
		UPDATE local_branches
		SET session_id = ?, session_pid = ?, session_status = ?, session_log = ?
		WHERE workspace_id = ?
	`, u.SessionID, u.SessionPID, u.SessionStatus, u.SessionLog, workspaceID)
	return err
}

// Scan discovers workspace directories from the configured root path,
// upserts them into DB, and removes entries for deleted directories.
func (m *Manager) Scan(ctx context.Context) (*ScanResult, error) {
	if m.cfg == nil || m.cfg.Root == "" {
		return &ScanResult{}, nil
	}
	entries, err := ScanDirectories(m.cfg.Root, m.cfg.RepoName)
	if err != nil {
		return nil, err
	}

	existing, _ := m.List(ctx)
	existingMap := make(map[string]bool, len(existing))
	for _, b := range existing {
		existingMap[b.WorkspaceID] = true
	}

	scannedIDs := make(map[string]bool, len(entries))
	added := 0
	for _, e := range entries {
		scannedIDs[e.WorkspaceID] = true
		if !existingMap[e.WorkspaceID] {
			added++
		}
		if err := m.Upsert(ctx, &LocalBranch{
			WorkspaceID: e.WorkspaceID,
			Directory:   e.Directory,
			GitBranch:   e.GitBranch,
			GitStatus:   e.GitStatus,
		}); err != nil {
			ml.Warn("scan upsert failed", "workspace", e.WorkspaceID, "err", err)
		}
	}

	removed := 0
	for _, b := range existing {
		if !scannedIDs[b.WorkspaceID] {
			if err := m.Delete(ctx, b.WorkspaceID); err != nil {
				ml.Warn("scan delete failed", "workspace", b.WorkspaceID, "err", err)
			}
			removed++
		}
	}

	// Detect active Claude Code sessions via .jsonl mtime fallback.
	// This catches sessions that started before the SessionStart hook was installed.
	var dirs []string
	for _, e := range entries {
		dirs = append(dirs, e.Directory)
	}
	activeSessions := DetectActiveClaudeSessions(dirs, 5*time.Minute)
	for dir := range activeSessions {
		wsID := ""
		for _, e := range entries {
			if e.Directory == dir {
				wsID = e.WorkspaceID
				break
			}
		}
		if wsID == "" {
			continue
		}
		// Only promote STOP → EXTERNAL (don't override MANAGED).
		// Use IncrementExternalSession (not UpdateSession) to correctly set ref count to 1.
		b, err := m.Get(ctx, wsID)
		if err != nil || b.SessionStatus != "STOP" {
			continue
		}
		if err := m.IncrementExternalSession(ctx, wsID); err == nil {
			ml.Info("detected active Claude session via mtime", "workspace", wsID)
		}
	}

	// Reap zombie EXTERNAL sessions (e.g., SessionEnd hook was never called)
	m.ReapZombieSessions(ctx)

	return &ScanResult{Added: added, Removed: removed}, nil
}

// IncrementExternalSession bumps the ref count and sets status to EXTERNAL.
// session_pid is reused as ref count for EXTERNAL sessions.
func (m *Manager) IncrementExternalSession(ctx context.Context, wsID string) error {
	_, err := m.q.Exec(ctx, `
		UPDATE local_branches
		SET session_status = 'EXTERNAL',
			session_pid = CASE WHEN session_status = 'EXTERNAL' THEN session_pid + 1 ELSE 1 END
		WHERE workspace_id = ?
	`, wsID)
	return err
}

// DecrementExternalSession decrements the ref count. Reverts to STOP when it reaches zero.
// Does nothing if the session is not EXTERNAL (e.g., MANAGED sessions are untouched).
// NOTE: SQLite evaluates SET using pre-update row values, so session_pid in the CASE
// refers to the value before decrement. pid=1 → STOP is correct (will become 0).
func (m *Manager) DecrementExternalSession(ctx context.Context, wsID string) error {
	_, err := m.q.Exec(ctx, `
		UPDATE local_branches
		SET session_pid = MAX(session_pid - 1, 0),
			session_status = CASE WHEN session_pid <= 1 THEN 'STOP' ELSE 'EXTERNAL' END
		WHERE workspace_id = ? AND session_status = 'EXTERNAL'
	`, wsID)
	return err
}

// FindWorkspaceByDir looks up workspace_id by its directory path.
func (m *Manager) FindWorkspaceByDir(ctx context.Context, dir string) (string, error) {
	row := m.q.QueryRow(ctx, `
		SELECT workspace_id FROM local_branches WHERE directory = ?
	`, dir)
	var wsID string
	if err := row.Scan(&wsID); err != nil {
		return "", fmt.Errorf("workspace not found for directory %s: %w", dir, err)
	}
	return wsID, nil
}

// ReapZombieSessions resets EXTERNAL sessions to STOP when no claude.exe
// process is running. Called during Scan to clean up stale entries left by
// sessions that exited without triggering the SessionEnd hook.
func (m *Manager) ReapZombieSessions(ctx context.Context) int {
	if !hasClaudeProcess() {
		// No claude.exe running at all — clear all EXTERNAL sessions
		res, err := m.q.Exec(ctx, `
			UPDATE local_branches SET session_status = 'STOP', session_pid = 0
			WHERE session_status = 'EXTERNAL'
		`)
		if err != nil {
			ml.Warn("reap zombie sessions failed", "err", err)
			return 0
		}
		n, _ := res.RowsAffected()
		if n > 0 {
			ml.Info("reaped zombie EXTERNAL sessions", "count", n)
		}
		return int(n)
	}
	return 0
}

// DashboardBranch is a LocalBranch enriched with handoff and backlog info for the dashboard.
type DashboardBranch struct {
	LocalBranch
	HandoffStatus   string `json:"handoff_status,omitempty"`
	BacklogIDs      string `json:"backlog_ids,omitempty"`
	SessionRefLabel string `json:"session_ref_label,omitempty"` // "×2", "×3", etc.
}

// BlockedBacklog holds a backlog item that is FIXING with a non-empty blocked_reason.
type BlockedBacklog struct {
	ID            int    `json:"id"`
	Title         string `json:"title"`
	BlockedReason string `json:"blocked_reason"`
}

// DashboardBranchesList returns all local branches LEFT JOINed with handoff + backlog data.
func (m *Manager) DashboardBranchesList(ctx context.Context) ([]DashboardBranch, error) {
	rows, err := m.q.Query(ctx, `
		SELECT lb.workspace_id, lb.directory,
			COALESCE(lb.git_branch,''), COALESCE(lb.git_status,'UNKNOWN'),
			COALESCE(lb.session_id,''), COALESCE(lb.session_pid,0), COALESCE(lb.session_status,'STOP'),
			COALESCE(lb.session_log,''), COALESCE(lb.last_scanned,''), COALESCE(lb.created_at,''),
			COALESCE(ab.status,''),
			COALESCE(
				(SELECT GROUP_CONCAT(bb.backlog_id) FROM branch_backlogs bb WHERE bb.branch = ab.branch),
				''
			)
		FROM local_branches lb
		LEFT JOIN active_branches ab ON lb.workspace_id = ab.branch
		ORDER BY
			CASE WHEN lb.workspace_id GLOB 'branch_[0-9]*' THEN 1 ELSE 0 END,
			CASE WHEN lb.workspace_id GLOB 'branch_[0-9]*'
				THEN CAST(REPLACE(lb.workspace_id, 'branch_', '') AS INTEGER)
				ELSE 0 END,
			lb.workspace_id
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []DashboardBranch
	for rows.Next() {
		var db DashboardBranch
		if err := rows.Scan(
			&db.WorkspaceID, &db.Directory, &db.GitBranch, &db.GitStatus,
			&db.SessionID, &db.SessionPID, &db.SessionStatus, &db.SessionLog,
			&db.LastScanned, &db.CreatedAt,
			&db.HandoffStatus, &db.BacklogIDs,
		); err != nil {
			return nil, err
		}
		if db.SessionStatus == "EXTERNAL" && db.SessionPID > 1 {
			db.SessionRefLabel = fmt.Sprintf("×%d", db.SessionPID)
		}
		result = append(result, db)
	}
	return result, rows.Err()
}

// DashboardBlockedBacklogs returns FIXING backlogs with non-empty blocked_reason for the given IDs.
func (m *Manager) DashboardBlockedBacklogs(ctx context.Context, backlogIDs string) ([]BlockedBacklog, error) {
	if backlogIDs == "" {
		return nil, nil
	}
	// backlogIDs is comma-separated from GROUP_CONCAT — use IN with split.
	query := fmt.Sprintf(`
		SELECT id, COALESCE(title,''), COALESCE(blocked_reason,'')
		FROM backlog_items
		WHERE id IN (%s) AND status = 'FIXING'
			AND blocked_reason IS NOT NULL AND blocked_reason != ''
	`, backlogIDs) // Safe: backlogIDs is GROUP_CONCAT of integer IDs from DB
	rows, err := m.q.Query(ctx, query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []BlockedBacklog
	for rows.Next() {
		var b BlockedBacklog
		if err := rows.Scan(&b.ID, &b.Title, &b.BlockedReason); err != nil {
			return nil, err
		}
		result = append(result, b)
	}
	return result, rows.Err()
}

// DashboardBlockedCount returns the number of FIXING backlogs with a non-empty blocked_reason.
func (m *Manager) DashboardBlockedCount(ctx context.Context) (int, error) {
	row := m.q.QueryRow(ctx, `
		SELECT COUNT(*) FROM backlog_items
		WHERE status = 'FIXING' AND blocked_reason IS NOT NULL AND blocked_reason != ''
	`)
	var count int
	err := row.Scan(&count)
	return count, err
}

// SyncBranch runs git fetch + pull on a branch directory (main only).
// Refuses to sync workspaces with an active session (EXTERNAL or MANAGED).
func (m *Manager) SyncBranch(ctx context.Context, workspaceID string) (string, error) {
	b, err := m.Get(ctx, workspaceID)
	if err != nil {
		return "", err
	}
	if b.SessionStatus == "EXTERNAL" || b.SessionStatus == "MANAGED" {
		return "", fmt.Errorf("sync blocked: workspace %s has an active %s session", workspaceID, b.SessionStatus)
	}
	if b.GitBranch != "main" && b.GitBranch != "master" {
		return "", fmt.Errorf("sync only available on main/master branch, current: %s", b.GitBranch)
	}
	cmd := exec.CommandContext(ctx, "git", "fetch", "origin", "main")
	cmd.Dir = b.Directory
	if out, err := cmd.CombinedOutput(); err != nil {
		return string(out), fmt.Errorf("fetch: %w: %s", err, strings.TrimSpace(string(out)))
	}
	cmd = exec.CommandContext(ctx, "git", "pull", "origin", "main")
	cmd.Dir = b.Directory
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}
