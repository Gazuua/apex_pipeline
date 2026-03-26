// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workspace

import (
	"context"
	"fmt"
	"os/exec"
	"strings"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

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
		_ = m.Upsert(ctx, &LocalBranch{
			WorkspaceID: e.WorkspaceID,
			Directory:   e.Directory,
			GitBranch:   e.GitBranch,
			GitStatus:   e.GitStatus,
		})
	}

	removed := 0
	for _, b := range existing {
		if !scannedIDs[b.WorkspaceID] {
			_ = m.Delete(ctx, b.WorkspaceID)
			removed++
		}
	}

	return &ScanResult{Added: added, Removed: removed}, nil
}

// SyncBranch runs git fetch + pull on a branch directory (main only).
func (m *Manager) SyncBranch(ctx context.Context, workspaceID string) (string, error) {
	b, err := m.Get(ctx, workspaceID)
	if err != nil {
		return "", err
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
