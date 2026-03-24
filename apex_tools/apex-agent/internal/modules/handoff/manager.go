// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

var ml = log.WithModule("handoff")

// Branch represents a registered working branch.
type Branch struct {
	Branch     string
	Workspace  string
	GitBranch  string // git branch name
	Status     string
	BacklogIDs []int // junction 테이블에서 조회
	Summary    string
	CreatedAt  string
	UpdatedAt  string
}

// ActiveBranchInfo is a lightweight view for list-active queries.
type ActiveBranchInfo struct {
	Branch    string `json:"branch"`
	GitBranch string `json:"git_branch"`
}

// BacklogOperator is the interface handoff needs from backlog.
type BacklogOperator interface {
	SetStatus(id int, status string) error
	SetStatusWith(q store.Querier, id int, status string) error
	Check(id int) (exists bool, status string, err error)
	ListFixingForBranch(branch string, backlogIDs []int) ([]int, error)
}

// Manager provides handoff business logic.
type Manager struct {
	store          *store.Store
	backlogManager BacklogOperator
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store, bm BacklogOperator) *Manager {
	return &Manager{store: s, backlogManager: bm}
}

// NotifyStart registers a new branch.
// If skipDesign is true, sets status to "implementing" directly.
// gitBranch is stored for fallback lookups by hook system.
func (m *Manager) NotifyStart(branch, workspace, summary, gitBranch string, backlogIDs []int, scopes string, skipDesign bool) error {
	status := StatusStarted
	if skipDesign {
		status = StatusImplementing
	}

	// FIXING 중복 체크 (early reject — 읽기 전용).
	if m.backlogManager != nil {
		for _, bid := range backlogIDs {
			if bid == 0 {
				continue
			}
			exists, bStatus, checkErr := m.backlogManager.Check(bid)
			if checkErr != nil {
				return fmt.Errorf("check backlog %d: %w", bid, checkErr)
			}
			if !exists {
				return fmt.Errorf("backlog item %d not found", bid)
			}
			if bStatus == "FIXING" {
				return fmt.Errorf("backlog item %d is already FIXING", bid)
			}
		}
	}

	err := m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		// 기존 항목이 있으면 정리 후 재등록 허용
		var existingStatus string
		row := tx.QueryRow(`SELECT status FROM active_branches WHERE branch = ?`, branch)
		if scanErr := row.Scan(&existingStatus); scanErr == nil {
			// 이전 작업에 연결된 FIXING 백로그가 있으면 OPEN으로 복귀
			if m.backlogManager != nil {
				oldIDs, _ := m.getBacklogIDs(tx, branch)
				for _, oldID := range oldIDs {
					if releaseErr := m.backlogManager.SetStatusWith(tx, oldID, "OPEN"); releaseErr != nil {
						ml.Warn("failed to release backlog on branch replace", "backlog_id", oldID, "err", releaseErr)
					}
				}
			}
			// 이전 작업의 잔여 데이터 정리
			if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
				return fmt.Errorf("delete branch_backlogs: %w", err)
			}
			if _, err := tx.Exec(`DELETE FROM active_branches WHERE branch = ?`, branch); err != nil {
				return fmt.Errorf("delete active_branches: %w", err)
			}
			ml.Info("cleared stale entry for re-registration", "branch", branch, "previous_status", existingStatus)
		}

		_, err := tx.Exec(
			`INSERT INTO active_branches (branch, workspace, git_branch, status, summary, created_at, updated_at)
			 VALUES (?, ?, ?, ?, ?, datetime('now','localtime'), datetime('now','localtime'))`,
			branch, workspace, store.NullableString(gitBranch), status, summary,
		)
		if err != nil {
			return fmt.Errorf("insert branch: %w", err)
		}

		// junction 테이블에 백로그 연결
		for _, bid := range backlogIDs {
			if bid == 0 {
				continue
			}
			_, err = tx.Exec(
				`INSERT INTO branch_backlogs (branch, backlog_id) VALUES (?, ?)`,
				branch, bid,
			)
			if err != nil {
				return fmt.Errorf("insert branch_backlog %d: %w", bid, err)
			}
		}

		// FIXING 상태 전이 — 트랜잭션 내에서 실행하여 원자성 보장
		if m.backlogManager != nil {
			for _, bid := range backlogIDs {
				if bid == 0 {
					continue
				}
				if err := m.backlogManager.SetStatusWith(tx, bid, "FIXING"); err != nil {
					return fmt.Errorf("backlog FIXING 전이 실패 (backlog_id=%d): %w", bid, err)
				}
			}
		}

		return nil
	})
	if err != nil {
		return err
	}

	ml.Audit("branch registered", "branch", branch, "workspace", workspace, "status", status)
	return nil
}

// NotifyTransition applies a state transition (design/plan).
// Note: merge/drop are handled by NotifyMerge/NotifyDrop, not through NotifyTransition.
// Read-validate-update is wrapped in a transaction to prevent TOCTOU races.
func (m *Manager) NotifyTransition(branch, workspace, notifyType, summary string) error {
	var fromStatus, toStatus string

	err := m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		// Read current status within transaction
		row := tx.QueryRow(`SELECT status FROM active_branches WHERE branch = ?`, branch)
		var currentStatus string
		if scanErr := row.Scan(&currentStatus); scanErr != nil {
			if errors.Is(scanErr, sql.ErrNoRows) {
				return fmt.Errorf("branch %q is not registered", branch)
			}
			return fmt.Errorf("get status: %w", scanErr)
		}

		nextStatus, nsErr := NextStatus(currentStatus, notifyType)
		if nsErr != nil {
			return nsErr
		}

		_, updErr := tx.Exec(
			`UPDATE active_branches SET status = ?, updated_at = datetime('now','localtime') WHERE branch = ?`,
			nextStatus, branch,
		)
		if updErr != nil {
			return fmt.Errorf("update branch status: %w", updErr)
		}

		fromStatus, toStatus = currentStatus, nextStatus
		return nil
	})
	if err != nil {
		return err
	}

	ml.Audit("state transition", "branch", branch, "from", fromStatus, "to", toStatus, "type", notifyType)
	return nil
}

// ── Merge / Drop ──────────────────────────────────────────────────────────────

// checkFixingBacklogs returns FIXING backlog IDs linked to the given branch.
func (m *Manager) checkFixingBacklogs(branch string) ([]int, error) {
	if m.backlogManager == nil {
		return nil, nil
	}
	rows, err := m.store.Query(
		`SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch,
	)
	if err != nil {
		return nil, fmt.Errorf("query branch_backlogs: %w", err)
	}
	defer rows.Close()
	var backlogIDs []int
	for rows.Next() {
		var id int
		if scanErr := rows.Scan(&id); scanErr != nil {
			return nil, fmt.Errorf("scan branch_backlog id: %w", scanErr)
		}
		backlogIDs = append(backlogIDs, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate branch_backlogs: %w", err)
	}
	return m.backlogManager.ListFixingForBranch(branch, backlogIDs)
}

// finalizeBranch moves an active branch to history and cleans up related records.
func (m *Manager) finalizeBranch(branch, workspace, summary, historyStatus string) error {
	return m.store.RunInTx(context.Background(), func(tx *store.TxStore) error {
		// 현재 브랜치 정보 조회
		var b Branch
		var dbSummary sql.NullString
		row := tx.QueryRow(
			`SELECT branch, workspace, COALESCE(git_branch,''), status, summary, created_at
			 FROM active_branches WHERE branch = ?`, branch)
		if err := row.Scan(&b.Branch, &b.Workspace, &b.GitBranch, &b.Status, &dbSummary, &b.CreatedAt); err != nil {
			return fmt.Errorf("branch not found: %w", err)
		}

		// backlog IDs 스냅샷
		backlogIDs, idsErr := m.getBacklogIDs(tx, branch)
		if idsErr != nil {
			return fmt.Errorf("snapshot backlog IDs: %w", idsErr)
		}
		backlogJSON, err := json.Marshal(backlogIDs)
		if err != nil {
			return fmt.Errorf("marshal backlog_ids: %w", err)
		}

		// branch_history 삽입 — merge/drop 시 전달받은 summary 우선, 없으면 DB의 기존 summary fallback
		historySummary := summary
		if historySummary == "" {
			historySummary = dbSummary.String
		}
		if _, err := tx.Exec(
			`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, backlog_ids, started_at)
			 VALUES (?, ?, ?, ?, ?, ?, ?)`,
			b.Branch, b.Workspace, store.NullableString(b.GitBranch),
			historyStatus, historySummary, string(backlogJSON), b.CreatedAt,
		); err != nil {
			return fmt.Errorf("insert branch_history: %w", err)
		}

		// 관련 데이터 정리
		if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
			return fmt.Errorf("delete branch_backlogs: %w", err)
		}
		if _, err := tx.Exec(`DELETE FROM active_branches WHERE branch = ?`, branch); err != nil {
			return fmt.Errorf("delete active_branches: %w", err)
		}

		return nil
	})
}

// requireNoFixingBacklogs checks that no FIXING backlogs remain for the branch.
func (m *Manager) requireNoFixingBacklogs(branch string) error {
	fixingIDs, err := m.checkFixingBacklogs(branch)
	if err != nil {
		return err
	}
	if len(fixingIDs) > 0 {
		return fmt.Errorf("FIXING 상태 백로그가 남아있습니다: %v\n먼저 backlog resolve 또는 release로 처리하세요", fixingIDs)
	}
	return nil
}

// NotifyMerge completes a branch — moves to history as MERGED.
// Blocks if FIXING backlogs remain.
func (m *Manager) NotifyMerge(branch, workspace, summary string) error {
	if err := m.requireNoFixingBacklogs(branch); err != nil {
		return err
	}
	if err := m.finalizeBranch(branch, workspace, summary, HistoryMerged); err != nil {
		return err
	}
	ml.Audit("branch merged", "branch", branch)
	return nil
}

// NotifyDrop abandons a branch — moves to history as DROPPED.
// Blocks if FIXING backlogs remain.
func (m *Manager) NotifyDrop(branch, workspace, reason string) error {
	if err := m.requireNoFixingBacklogs(branch); err != nil {
		return err
	}
	if err := m.finalizeBranch(branch, workspace, reason, HistoryDropped); err != nil {
		return err
	}
	ml.Audit("branch dropped", "branch", branch, "reason", reason)
	return nil
}

// ListActive returns all active branches (for cleanup integration).
func (m *Manager) ListActive() ([]ActiveBranchInfo, error) {
	rows, err := m.store.Query(`SELECT branch, COALESCE(git_branch,'') FROM active_branches`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var result []ActiveBranchInfo
	for rows.Next() {
		var info ActiveBranchInfo
		if err := rows.Scan(&info.Branch, &info.GitBranch); err != nil {
			return nil, err
		}
		result = append(result, info)
	}
	return result, rows.Err()
}

// ── Queries ───────────────────────────────────────────────────────────────────

// BacklogCheck checks if a backlog item is being worked on by any active branch.
// All records in active_branches are active, so no status filter needed.
func (m *Manager) BacklogCheck(backlogID int) (available bool, branch string, err error) {
	row := m.store.QueryRow(
		`SELECT bb.branch FROM branch_backlogs bb
		 JOIN active_branches b ON b.branch = bb.branch
		 WHERE bb.backlog_id = ?`,
		backlogID,
	)
	var b string
	if scanErr := row.Scan(&b); scanErr != nil {
		if errors.Is(scanErr, sql.ErrNoRows) {
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
		 FROM active_branches WHERE branch = ?`,
		branch,
	)

	var b Branch
	var summary sql.NullString
	if err := row.Scan(&b.Branch, &b.Workspace, &b.GitBranch, &b.Status, &summary, &b.CreatedAt, &b.UpdatedAt); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
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
		if scanErr := rows.Scan(&id); scanErr != nil {
			return nil, fmt.Errorf("scan backlog id: %w", scanErr)
		}
		b.BacklogIDs = append(b.BacklogIDs, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate backlog ids: %w", err)
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
			`SELECT branch FROM active_branches WHERE git_branch = ?`, gitBranch,
		)
		var branch string
		if scanErr := row.Scan(&branch); scanErr != nil {
			if !errors.Is(scanErr, sql.ErrNoRows) {
				return "", fmt.Errorf("resolve by git_branch: %w", scanErr)
			}
		} else {
			return branch, nil
		}
	}

	return "", nil
}

// GetStatus returns the current status of a branch (empty string if not registered).
func (m *Manager) GetStatus(branch string) (string, error) {
	row := m.store.QueryRow(
		`SELECT status FROM active_branches WHERE branch = ?`,
		branch,
	)
	var status string
	if err := row.Scan(&status); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return "", nil
		}
		return "", fmt.Errorf("scan status: %w", err)
	}
	return status, nil
}

// getBacklogIDs returns backlog IDs linked to a branch via junction table.
func (m *Manager) getBacklogIDs(s store.Querier, branch string) ([]int, error) {
	rows, err := s.Query(`SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch)
	if err != nil {
		return nil, fmt.Errorf("query branch_backlogs for %s: %w", branch, err)
	}
	defer rows.Close()
	var ids []int
	for rows.Next() {
		var id int
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("scan backlog_id for %s: %w", branch, err)
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate backlog_ids for %s: %w", branch, err)
	}
	return ids, nil
}
