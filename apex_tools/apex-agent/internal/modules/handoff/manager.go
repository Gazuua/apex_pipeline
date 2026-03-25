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
	SetStatus(ctx context.Context, id int, status string) error
	SetStatusWith(ctx context.Context, q store.Querier, id int, status string) error
	Check(ctx context.Context, id int) (exists bool, status string, err error)
	ListFixingForBranch(ctx context.Context, branch string, backlogIDs []int) ([]int, error)
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
func (m *Manager) NotifyStart(ctx context.Context, branch, workspace, summary, gitBranch string, backlogIDs []int, scopes string, skipDesign bool) error {
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
			exists, bStatus, checkErr := m.backlogManager.Check(ctx, bid)
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

	err := m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		// 기존 항목이 있으면 정리 후 재등록 허용
		var existingStatus string
		row := tx.QueryRow(ctx, `SELECT status FROM active_branches WHERE branch = ?`, branch)
		if scanErr := row.Scan(&existingStatus); scanErr == nil {
			// 이전 작업에 연결된 FIXING 백로그가 있으면 OPEN으로 복귀
			// SetStatusWith의 DB 가드가 FIXING→OPEN만 허용하므로 RESOLVED 항목은 자동 스킵됨
			if m.backlogManager != nil {
				oldIDs, getErr := m.getBacklogIDs(ctx, tx, branch)
				if getErr != nil {
					return fmt.Errorf("get old backlog IDs for branch replace: %w", getErr)
				}
				for _, oldID := range oldIDs {
					if releaseErr := m.backlogManager.SetStatusWith(ctx, tx, oldID, "OPEN"); releaseErr != nil {
						// RESOLVED 등 비-FIXING 항목은 SetStatusWith가 거부 — 정상 스킵
						ml.Info("skipped backlog release on branch replace (not FIXING)", "id", oldID, "error", releaseErr)
					}
				}
			}
			// 이전 작업의 잔여 데이터 정리
			if _, err := tx.Exec(ctx, `DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
				return fmt.Errorf("delete branch_backlogs: %w", err)
			}
			if _, err := tx.Exec(ctx, `DELETE FROM active_branches WHERE branch = ?`, branch); err != nil {
				return fmt.Errorf("delete active_branches: %w", err)
			}
			ml.Info("cleared stale entry for re-registration", "branch", branch, "previous_status", existingStatus)
		}

		_, err := tx.Exec(ctx,
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
			_, err = tx.Exec(ctx,
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
				if err := m.backlogManager.SetStatusWith(ctx, tx, bid, "FIXING"); err != nil {
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
func (m *Manager) NotifyTransition(ctx context.Context, branch, workspace, notifyType, summary string) error {
	var fromStatus, toStatus string

	err := m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		// Read current status within transaction
		row := tx.QueryRow(ctx, `SELECT status FROM active_branches WHERE branch = ?`, branch)
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

		_, updErr := tx.Exec(ctx,
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
func (m *Manager) checkFixingBacklogs(ctx context.Context, branch string) ([]int, error) {
	if m.backlogManager == nil {
		return nil, nil
	}
	rows, err := m.store.Query(ctx,
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
	return m.backlogManager.ListFixingForBranch(ctx, branch, backlogIDs)
}

// finalizeBranch moves an active branch to history and cleans up related records.
// FIXING backlog check is performed inside the transaction to prevent TOCTOU races.
// If releaseFixing is true, FIXING backlogs are released to OPEN within the same transaction (used by drop).
func (m *Manager) finalizeBranch(ctx context.Context, branch, workspace, summary, historyStatus string, checkFixing, releaseFixing bool) error {
	return m.store.RunInTx(ctx, func(tx *store.TxStore) error {
		// FIXING 백로그 체크 (트랜잭션 내에서 원자적 검증)
		if checkFixing {
			if err := m.requireNoFixingBacklogsTx(ctx, tx, branch); err != nil {
				return err
			}
		}

		// Drop 시 FIXING 백로그를 OPEN으로 복귀 (단일 TX 원자성 보장)
		// SetStatusWith의 DB 가드가 FIXING→OPEN만 허용하므로 RESOLVED 항목은 자동 스킵됨
		if releaseFixing && m.backlogManager != nil {
			ids, idsErr := m.getBacklogIDs(ctx, tx, branch)
			if idsErr != nil {
				return fmt.Errorf("get backlog IDs for release: %w", idsErr)
			}
			for _, id := range ids {
				if err := m.backlogManager.SetStatusWith(ctx, tx, id, "OPEN"); err != nil {
					// RESOLVED 등 비-FIXING 항목은 SetStatusWith가 거부 — 정상 스킵
					ml.Info("skipped backlog release on drop (not FIXING)", "id", id, "error", err)
				}
			}
		}

		// 현재 브랜치 정보 조회
		var b Branch
		var dbSummary sql.NullString
		row := tx.QueryRow(ctx,
			`SELECT branch, workspace, COALESCE(git_branch,''), status, summary, created_at
			 FROM active_branches WHERE branch = ?`, branch)
		if err := row.Scan(&b.Branch, &b.Workspace, &b.GitBranch, &b.Status, &dbSummary, &b.CreatedAt); err != nil {
			return fmt.Errorf("branch not found: %w", err)
		}

		// backlog IDs 스냅샷
		backlogIDs, idsErr := m.getBacklogIDs(ctx, tx, branch)
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
		if _, err := tx.Exec(ctx,
			`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, backlog_ids, started_at)
			 VALUES (?, ?, ?, ?, ?, ?, ?)`,
			b.Branch, b.Workspace, store.NullableString(b.GitBranch),
			historyStatus, historySummary, string(backlogJSON), b.CreatedAt,
		); err != nil {
			return fmt.Errorf("insert branch_history: %w", err)
		}

		// 관련 데이터 정리
		if _, err := tx.Exec(ctx, `DELETE FROM branch_backlogs WHERE branch = ?`, branch); err != nil {
			return fmt.Errorf("delete branch_backlogs: %w", err)
		}
		if _, err := tx.Exec(ctx, `DELETE FROM active_branches WHERE branch = ?`, branch); err != nil {
			return fmt.Errorf("delete active_branches: %w", err)
		}

		return nil
	})
}

// requireNoFixingBacklogsTx checks that no FIXING backlogs remain for the branch (transaction-safe).
func (m *Manager) requireNoFixingBacklogsTx(ctx context.Context, tx *store.TxStore, branch string) error {
	if m.backlogManager == nil {
		return nil
	}
	rows, err := tx.Query(ctx, `SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch)
	if err != nil {
		return fmt.Errorf("query branch_backlogs: %w", err)
	}
	defer rows.Close()
	var backlogIDs []int
	for rows.Next() {
		var id int
		if scanErr := rows.Scan(&id); scanErr != nil {
			return fmt.Errorf("scan branch_backlog id: %w", scanErr)
		}
		backlogIDs = append(backlogIDs, id)
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("iterate branch_backlogs: %w", err)
	}
	fixingIDs, err := m.backlogManager.ListFixingForBranch(ctx, branch, backlogIDs)
	if err != nil {
		return err
	}
	if len(fixingIDs) > 0 {
		return fmt.Errorf("FIXING 상태 백로그가 남아있습니다: %v\n먼저 backlog resolve 또는 release로 처리하세요", fixingIDs)
	}
	return nil
}

// NotifyMerge completes a branch — moves to history as MERGED.
// FIXING check + finalize are in a single transaction to prevent TOCTOU.
func (m *Manager) NotifyMerge(ctx context.Context, branch, workspace, summary string) error {
	if err := m.finalizeBranch(ctx, branch, workspace, summary, HistoryMerged, true, false); err != nil {
		return err
	}
	ml.Audit("branch merged", "branch", branch)
	return nil
}

// NotifyDrop abandons a branch — auto-releases FIXING backlogs to OPEN, then moves to history as DROPPED.
// FIXING release + finalize are in a single transaction to guarantee atomicity.
func (m *Manager) NotifyDrop(ctx context.Context, branch, workspace, reason string) error {
	if err := m.finalizeBranch(ctx, branch, workspace, reason, HistoryDropped, false, true); err != nil {
		return err
	}
	ml.Audit("branch dropped", "branch", branch, "reason", reason)
	return nil
}

// ListActive returns all active branches (for cleanup integration).
func (m *Manager) ListActive(ctx context.Context) ([]ActiveBranchInfo, error) {
	rows, err := m.store.Query(ctx, `SELECT branch, COALESCE(git_branch,'') FROM active_branches`)
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
func (m *Manager) BacklogCheck(ctx context.Context, backlogID int) (available bool, branch string, err error) {
	row := m.store.QueryRow(ctx,
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
func (m *Manager) GetBranch(ctx context.Context, branch string) (*Branch, error) {
	row := m.store.QueryRow(ctx,
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
	rows, err := m.store.Query(ctx,
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
func (m *Manager) ResolveBranch(ctx context.Context, workspaceID, gitBranch string) (string, error) {
	// 1차: workspace ID로 조회
	status, err := m.GetStatus(ctx, workspaceID)
	if err != nil {
		return "", err
	}
	if status != "" {
		return workspaceID, nil
	}

	// 2차: git branch 이름으로 fallback
	if gitBranch != "" {
		row := m.store.QueryRow(ctx,
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
func (m *Manager) GetStatus(ctx context.Context, branch string) (string, error) {
	row := m.store.QueryRow(ctx,
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

// ── Dashboard queries ─────────────────────────────────────────────────────────

// DashboardActiveBranch is a view for dashboard active branches display.
type DashboardActiveBranch struct {
	Branch      string
	WorkspaceID string
	GitBranch   string
	Summary     string
	Status      string
	BacklogIDs  []int
	CreatedAt   string
}

// DashboardBranchHistory is a view for dashboard branch history display.
type DashboardBranchHistory struct {
	ID         int
	Branch     string
	GitBranch  string
	Summary    string
	Status     string
	BacklogIDs string
	StartedAt  string
	FinishedAt string
}

// DashboardActiveBranches returns all active branches with linked backlog IDs.
func (m *Manager) DashboardActiveBranches(ctx context.Context) ([]DashboardActiveBranch, error) {
	rows, err := m.store.Query(ctx, `
		SELECT branch, workspace, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, created_at
		FROM active_branches
		ORDER BY created_at DESC`)
	if err != nil {
		return nil, fmt.Errorf("DashboardActiveBranches: %w", err)
	}

	// Collect all branches first, then close rows before nested queries.
	// SQLite :memory: with MaxOpenConns(1) deadlocks on nested queries.
	var branches []DashboardActiveBranch
	for rows.Next() {
		var ab DashboardActiveBranch
		if err := rows.Scan(&ab.Branch, &ab.WorkspaceID, &ab.GitBranch, &ab.Summary,
			&ab.Status, &ab.CreatedAt); err != nil {
			rows.Close()
			return nil, fmt.Errorf("DashboardActiveBranches scan: %w", err)
		}
		branches = append(branches, ab)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("DashboardActiveBranches rows: %w", err)
	}
	rows.Close()

	// Fetch linked backlog IDs per branch
	for i := range branches {
		bbRows, err := m.store.Query(ctx, `SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branches[i].Branch)
		if err != nil {
			continue
		}
		for bbRows.Next() {
			var bid int
			if scanErr := bbRows.Scan(&bid); scanErr != nil {
				continue
			}
			branches[i].BacklogIDs = append(branches[i].BacklogIDs, bid)
		}
		bbRows.Close()
	}

	return branches, nil
}

// DashboardActiveCount returns the number of active branches.
func (m *Manager) DashboardActiveCount(ctx context.Context) (int, error) {
	var count int
	if err := m.store.QueryRow(ctx, `SELECT COUNT(*) FROM active_branches`).Scan(&count); err != nil {
		return 0, fmt.Errorf("DashboardActiveCount: %w", err)
	}
	return count, nil
}

// DashboardBranchHistoryList returns recent branch history entries.
func (m *Manager) DashboardBranchHistoryList(ctx context.Context, limit int) ([]DashboardBranchHistory, error) {
	rows, err := m.store.Query(ctx, `
		SELECT id, branch, COALESCE(git_branch,''), COALESCE(summary,''),
		       status, COALESCE(backlog_ids,''), started_at, finished_at
		FROM branch_history
		ORDER BY finished_at DESC
		LIMIT ?`, limit)
	if err != nil {
		return nil, fmt.Errorf("DashboardBranchHistoryList: %w", err)
	}
	defer rows.Close()

	var history []DashboardBranchHistory
	for rows.Next() {
		var bh DashboardBranchHistory
		if err := rows.Scan(&bh.ID, &bh.Branch, &bh.GitBranch, &bh.Summary,
			&bh.Status, &bh.BacklogIDs, &bh.StartedAt, &bh.FinishedAt); err != nil {
			return nil, fmt.Errorf("DashboardBranchHistoryList scan: %w", err)
		}
		history = append(history, bh)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("DashboardBranchHistoryList rows: %w", err)
	}
	return history, nil
}

// getBacklogIDs returns backlog IDs linked to a branch via junction table.
func (m *Manager) getBacklogIDs(ctx context.Context, s store.Querier, branch string) ([]int, error) {
	rows, err := s.Query(ctx, `SELECT backlog_id FROM branch_backlogs WHERE branch = ?`, branch)
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
