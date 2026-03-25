// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"fmt"
	"os"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"
)

// Module implements the daemon.Module interface for handoff management.
type Module struct {
	manager       *Manager
	backlogSyncer workflow.BacklogSyncer // for SyncExport/SyncImport in merge pipeline
}

// New creates a new handoff Module backed by the given store.
// syncer is the backlog syncer (for export/import in merge pipeline); bm is the operator interface.
func New(s *store.Store, bm BacklogOperator, qm QueueOperator, syncer workflow.BacklogSyncer) *Module {
	return &Module{
		manager:       NewManager(s, bm, qm),
		backlogSyncer: syncer,
	}
}

func (m *Module) Name() string { return "handoff" }

// Manager returns the underlying Manager for cross-module use.
func (m *Module) Manager() *Manager { return m.manager }

// RegisterSchema registers the branches, notifications, and notification_acks table migrations.
func (m *Module) RegisterSchema(mig *store.Migrator) {
	mig.Register("handoff", 1, func(tx *store.TxStore) error {
		queries := []string{
			`CREATE TABLE branches (
				branch      TEXT    PRIMARY KEY,
				workspace   TEXT    NOT NULL,
				status      TEXT    NOT NULL,
				backlog_id  INTEGER,
				summary     TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			)`,
			`CREATE TABLE notifications (
				id          INTEGER PRIMARY KEY AUTOINCREMENT,
				branch      TEXT    NOT NULL,
				workspace   TEXT    NOT NULL,
				type        TEXT    NOT NULL,
				summary     TEXT,
				payload     TEXT,
				created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
			)`,
			`CREATE TABLE notification_acks (
				notification_id INTEGER NOT NULL,
				branch          TEXT    NOT NULL,
				action          TEXT    NOT NULL,
				acked_at        TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
				PRIMARY KEY (notification_id, branch)
			)`,
			`CREATE INDEX idx_notifications_branch ON notifications(branch)`,
			`CREATE INDEX idx_acks_branch ON notification_acks(branch)`,
		}
		for _, q := range queries {
			if _, err := tx.Exec(context.Background(), q); err != nil {
				return err
			}
		}
		return nil
	})
	mig.Register("handoff", 2, func(tx *store.TxStore) error {
		// 1. junction 테이블 생성
		_, err := tx.Exec(context.Background(), `CREATE TABLE branch_backlogs (
			branch     TEXT    NOT NULL,
			backlog_id INTEGER NOT NULL,
			PRIMARY KEY (branch, backlog_id)
		)`)
		if err != nil {
			return fmt.Errorf("create branch_backlogs: %w", err)
		}

		// 2. 기존 branches.backlog_id 데이터 이관 (NULL 스킵)
		_, err = tx.Exec(context.Background(), `
			INSERT INTO branch_backlogs (branch, backlog_id)
			SELECT branch, backlog_id FROM branches WHERE backlog_id IS NOT NULL
		`)
		if err != nil {
			return fmt.Errorf("migrate backlog_ids: %w", err)
		}

		// 3. branches 재생성 (backlog_id 컬럼 제거)
		// 개별 Exec()로 분리 — partial failure 시 명확한 에러 반환
		if _, err = tx.Exec(context.Background(), `CREATE TABLE branches_v2 (
			branch      TEXT    PRIMARY KEY,
			workspace   TEXT    NOT NULL,
			status      TEXT    NOT NULL,
			summary     TEXT,
			created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`); err != nil {
			return fmt.Errorf("create branches_v2: %w", err)
		}
		if _, err = tx.Exec(context.Background(), `INSERT INTO branches_v2 (branch, workspace, status, summary, created_at, updated_at)
			SELECT branch, workspace, status, summary, created_at, updated_at FROM branches`); err != nil {
			return fmt.Errorf("copy to branches_v2: %w", err)
		}
		if _, err = tx.Exec(context.Background(), `DROP TABLE branches`); err != nil {
			return fmt.Errorf("drop old branches: %w", err)
		}
		if _, err = tx.Exec(context.Background(), `ALTER TABLE branches_v2 RENAME TO branches`); err != nil {
			return fmt.Errorf("rename branches_v2: %w", err)
		}
		return nil
	})
	mig.Register("handoff", 3, func(tx *store.TxStore) error {
		// git_branch 컬럼 추가 — hook fallback 조회용
		_, err := tx.Exec(context.Background(), `ALTER TABLE branches ADD COLUMN git_branch TEXT`)
		return err
	})
	mig.Register("handoff", 4, func(tx *store.TxStore) error {
		// status를 UPPER_SNAKE_CASE로 정규화
		_, err := tx.Exec(context.Background(), `UPDATE branches SET status = CASE status
			WHEN 'started' THEN 'STARTED'
			WHEN 'design-notified' THEN 'DESIGN_NOTIFIED'
			WHEN 'implementing' THEN 'IMPLEMENTING'
			WHEN 'merge-notified' THEN 'MERGE_NOTIFIED'
			ELSE UPPER(REPLACE(status, '-', '_'))
		END`)
		return err
	})
	mig.Register("handoff", 5, func(tx *store.TxStore) error {
		// 1. branches → active_branches 리네이밍
		if _, err := tx.Exec(context.Background(), `ALTER TABLE branches RENAME TO active_branches`); err != nil {
			return fmt.Errorf("rename branches: %w", err)
		}

		// 2. branch_history 테이블 생성
		if _, err := tx.Exec(context.Background(), `CREATE TABLE branch_history (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			branch      TEXT    NOT NULL,
			workspace   TEXT    NOT NULL,
			git_branch  TEXT,
			status      TEXT    NOT NULL,
			summary     TEXT,
			backlog_ids TEXT,
			started_at  TEXT    NOT NULL,
			finished_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`); err != nil {
			return fmt.Errorf("create branch_history: %w", err)
		}

		// 3. MERGE_NOTIFIED → branch_history (MERGED) 이관
		if _, err := tx.Exec(context.Background(), `INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
			SELECT branch, workspace, git_branch, 'MERGED', summary, created_at
			FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
			return fmt.Errorf("migrate merge_notified: %w", err)
		}
		if _, err := tx.Exec(context.Background(), `DELETE FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
			return fmt.Errorf("delete merge_notified: %w", err)
		}

		// 4. 나머지 전부 → branch_history (DROPPED) 이관
		if _, err := tx.Exec(context.Background(), `INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
			SELECT branch, workspace, git_branch, 'DROPPED', summary, created_at
			FROM active_branches`); err != nil {
			return fmt.Errorf("migrate remaining: %w", err)
		}
		if _, err := tx.Exec(context.Background(), `DELETE FROM active_branches`); err != nil {
			return fmt.Errorf("delete remaining: %w", err)
		}

		// 5. git_branch UNIQUE 제약 추가 (테이블 재생성)
		if _, err := tx.Exec(context.Background(), `CREATE TABLE active_branches_v2 (
			branch      TEXT PRIMARY KEY,
			workspace   TEXT NOT NULL,
			git_branch  TEXT UNIQUE,
			status      TEXT NOT NULL,
			summary     TEXT,
			created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
		)`); err != nil {
			return fmt.Errorf("create active_branches_v2: %w", err)
		}
		if _, err := tx.Exec(context.Background(), `DROP TABLE active_branches`); err != nil {
			return fmt.Errorf("drop active_branches: %w", err)
		}
		if _, err := tx.Exec(context.Background(), `ALTER TABLE active_branches_v2 RENAME TO active_branches`); err != nil {
			return fmt.Errorf("rename active_branches_v2: %w", err)
		}

		// 6. 고아 branch_backlogs 정리
		if _, err := tx.Exec(context.Background(), `DELETE FROM branch_backlogs WHERE branch NOT IN (SELECT branch FROM active_branches)`); err != nil {
			return fmt.Errorf("cleanup branch_backlogs: %w", err)
		}

		return nil
	})
	mig.Register("handoff", 6, func(tx *store.TxStore) error {
		// 알림 시스템 제거 — notifications/notification_acks 테이블 DROP
		for _, q := range []string{
			`DROP TABLE IF EXISTS notification_acks`,
			`DROP TABLE IF EXISTS notifications`,
			`DROP INDEX IF EXISTS idx_notifications_branch`,
			`DROP INDEX IF EXISTS idx_acks_branch`,
		} {
			if _, err := tx.Exec(context.Background(), q); err != nil {
				return fmt.Errorf("drop notification tables: %w", err)
			}
		}
		return nil
	})
}

// RegisterRoutes registers handoff action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("notify-start", daemon.Typed(m.handleNotifyStart))
	reg.Handle("notify-transition", daemon.Typed(m.handleNotifyTransition))
	reg.Handle("notify-merge", daemon.Typed(m.handleNotifyMerge))
	reg.Handle("notify-drop", daemon.Typed(m.handleNotifyDrop))
	reg.Handle("list-active", daemon.NoParams(m.handleListActive))
	reg.Handle("backlog-check", daemon.Typed(m.handleBacklogCheck))
	reg.Handle("get-branch", daemon.Typed(m.handleGetBranch))
	reg.Handle("get-status", daemon.Typed(m.handleGetStatus))
	reg.Handle("resolve-branch", daemon.Typed(m.handleResolveBranch))
	reg.Handle("validate-commit", daemon.Typed(m.handleValidateCommit))
	reg.Handle("validate-merge-gate", daemon.Typed(m.handleValidateMergeGate))
	reg.Handle("validate-edit", daemon.Typed(m.handleValidateEdit))
}

func (m *Module) OnStart(_ context.Context) error { return nil }
func (m *Module) OnStop() error                   { return nil }

// --- Request/Response types ---

type notifyStartParams struct {
	Branch     string `json:"branch"`
	Workspace  string `json:"workspace"`
	BranchName string `json:"branch_name"` // git 브랜치명 (브랜치 생성 전이므로 git_branch 대신 사용)
	Summary    string `json:"summary"`
	BacklogIDs []int  `json:"backlog_ids"`
	Scopes     string `json:"scopes"`
	SkipDesign bool   `json:"skip_design"`
}

type notifyTransitionParams struct {
	Branch    string `json:"branch"`
	Workspace string `json:"workspace"`
	Type      string `json:"type"`
	Summary   string `json:"summary"`
}

type backlogCheckParams struct {
	BacklogID int `json:"backlog_id"`
}

type getBranchParams struct {
	Branch string `json:"branch"`
}

type getStatusParams struct {
	Branch string `json:"branch"`
}

// --- Handlers ---

func (m *Module) handleNotifyStart(ctx context.Context, p notifyStartParams, _ string) (any, error) {
	if err := m.manager.NotifyStart(ctx, p.Branch, p.Workspace, p.Summary, p.BranchName, p.BacklogIDs, p.Scopes, p.SkipDesign); err != nil {
		return nil, err
	}
	return map[string]string{"status": "started"}, nil
}

func (m *Module) handleNotifyTransition(ctx context.Context, p notifyTransitionParams, _ string) (any, error) {
	if err := m.manager.NotifyTransition(ctx, p.Branch, p.Workspace, p.Type, p.Summary); err != nil {
		return nil, err
	}
	return map[string]string{"status": "transitioned"}, nil
}

func (m *Module) handleBacklogCheck(ctx context.Context, p backlogCheckParams, _ string) (any, error) {
	available, branch, err := m.manager.BacklogCheck(ctx, p.BacklogID)
	if err != nil {
		return nil, err
	}
	return map[string]any{"available": available, "branch": branch}, nil
}

func (m *Module) handleGetBranch(ctx context.Context, p getBranchParams, _ string) (any, error) {
	b, err := m.manager.GetBranch(ctx, p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"branch": b}, nil
}

func (m *Module) handleGetStatus(ctx context.Context, p getStatusParams, _ string) (any, error) {
	status, err := m.manager.GetStatus(ctx, p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"status": status}, nil
}

// --- Resolve handler ---

type resolveBranchParams struct {
	WorkspaceID string `json:"workspace_id"`
	GitBranch   string `json:"git_branch"`
}

func (m *Module) handleResolveBranch(ctx context.Context, p resolveBranchParams, _ string) (any, error) {
	branch, err := m.manager.ResolveBranch(ctx, p.WorkspaceID, p.GitBranch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"branch": branch, "found": branch != ""}, nil
}

// --- Gate handler request types ---

type validateCommitParams struct {
	Branch string `json:"branch"`
}

type validateMergeGateParams struct {
	Branch string `json:"branch"`
}

type validateEditParams struct {
	Branch   string `json:"branch"`
	FilePath string `json:"file_path"`
}

// --- Gate handlers ---

func (m *Module) handleValidateCommit(ctx context.Context, p validateCommitParams, _ string) (any, error) {
	if err := m.manager.ValidateCommit(ctx, p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

func (m *Module) handleValidateMergeGate(ctx context.Context, p validateMergeGateParams, _ string) (any, error) {
	if err := m.manager.ValidateMergeGate(ctx, p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

func (m *Module) handleValidateEdit(ctx context.Context, p validateEditParams, _ string) (any, error) {
	if err := m.manager.ValidateEdit(ctx, p.Branch, p.FilePath); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

// --- Merge / Drop / ListActive handlers ---

type notifyMergeParams struct {
	Branch      string `json:"branch"`
	Workspace   string `json:"workspace"`
	Summary     string `json:"summary"`
	ProjectRoot string `json:"project_root"` // 있으면 MergeFullPipeline 실행
}

func (m *Module) handleNotifyMerge(ctx context.Context, p notifyMergeParams, _ string) (any, error) {
	// project_root가 있으면 MergeFullPipeline 실행 (새 경로)
	if p.ProjectRoot != "" {
		mergeParams := m.buildMergeParams(ctx, p)
		if err := workflow.MergeFullPipeline(ctx, mergeParams); err != nil {
			return nil, err
		}
		return map[string]string{"status": "merged"}, nil
	}

	// Fallback: project_root 없으면 기존 동작 (DB finalize만)
	if err := m.manager.NotifyMerge(ctx, p.Branch, p.Workspace, p.Summary); err != nil {
		return nil, err
	}
	return map[string]string{"status": "merged"}, nil
}

// buildMergeParams assembles MergeFullParams from the parsed notifyMergeParams.
func (m *Module) buildMergeParams(ctx context.Context, p notifyMergeParams) workflow.MergeFullParams {
	return workflow.MergeFullParams{
		ProjectRoot: p.ProjectRoot,
		Branch:      p.Branch,
		Workspace:   p.Workspace,
		Summary:     p.Summary,
		PreMergeCheckFn: func(checkCtx context.Context) error {
			// ValidateMergeGate를 lock 획득 후 실행 — TOCTOU 윈도우 최소화.
			return m.manager.ValidateMergeGate(checkCtx, p.Branch)
		},
		ImportFn: func(root string) {
			if m.backlogSyncer != nil {
				if _, err := workflow.SyncImport(ctx, root, m.backlogSyncer); err != nil {
					ml.Warn("merge pipeline import 실패 (non-fatal)", "err", err)
				}
			}
		},
		ExportFn: func(root string) error {
			if m.backlogSyncer != nil {
				if _, err := workflow.SyncExport(ctx, root, m.backlogSyncer); err != nil {
					return err
				}
			}
			return nil
		},
		FinalizeFn: func(fCtx context.Context) error {
			return m.manager.NotifyMerge(fCtx, p.Branch, p.Workspace, p.Summary)
		},
		LockAcquireFn: func(lCtx context.Context) error {
			return m.manager.queueManager.Acquire(lCtx, "merge", p.Branch, os.Getpid())
		},
		LockReleaseFn: func(lCtx context.Context) error {
			return m.manager.queueManager.Release(lCtx, "merge")
		},
	}
}

type notifyDropParams struct {
	Branch    string `json:"branch"`
	Workspace string `json:"workspace"`
	Reason    string `json:"reason"`
}

func (m *Module) handleNotifyDrop(ctx context.Context, p notifyDropParams, _ string) (any, error) {
	if err := m.manager.NotifyDrop(ctx, p.Branch, p.Workspace, p.Reason); err != nil {
		return nil, err
	}
	return map[string]string{"status": "dropped"}, nil
}

func (m *Module) handleListActive(ctx context.Context, _ string) (any, error) {
	list, err := m.manager.ListActive(ctx)
	if err != nil {
		return nil, err
	}
	return map[string]any{"branches": list}, nil
}
