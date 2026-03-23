// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

// Module implements the daemon.Module interface for handoff management.
type Module struct {
	manager *Manager
}

// New creates a new handoff Module backed by the given store.
func New(s *store.Store, bm BacklogOperator) *Module {
	return &Module{manager: NewManager(s, bm)}
}

func (m *Module) Name() string { return "handoff" }

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
			if _, err := tx.Exec(q); err != nil {
				return err
			}
		}
		return nil
	})
	mig.Register("handoff", 2, func(tx *store.TxStore) error {
		// 1. junction 테이블 생성
		_, err := tx.Exec(`CREATE TABLE branch_backlogs (
			branch     TEXT    NOT NULL,
			backlog_id INTEGER NOT NULL,
			PRIMARY KEY (branch, backlog_id)
		)`)
		if err != nil {
			return fmt.Errorf("create branch_backlogs: %w", err)
		}

		// 2. 기존 branches.backlog_id 데이터 이관 (NULL 스킵)
		_, err = tx.Exec(`
			INSERT INTO branch_backlogs (branch, backlog_id)
			SELECT branch, backlog_id FROM branches WHERE backlog_id IS NOT NULL
		`)
		if err != nil {
			return fmt.Errorf("migrate backlog_ids: %w", err)
		}

		// 3. branches 재생성 (backlog_id 컬럼 제거)
		// 개별 Exec()로 분리 — partial failure 시 명확한 에러 반환
		if _, err = tx.Exec(`CREATE TABLE branches_v2 (
			branch      TEXT    PRIMARY KEY,
			workspace   TEXT    NOT NULL,
			status      TEXT    NOT NULL,
			summary     TEXT,
			created_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime')),
			updated_at  TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
		)`); err != nil {
			return fmt.Errorf("create branches_v2: %w", err)
		}
		if _, err = tx.Exec(`INSERT INTO branches_v2 (branch, workspace, status, summary, created_at, updated_at)
			SELECT branch, workspace, status, summary, created_at, updated_at FROM branches`); err != nil {
			return fmt.Errorf("copy to branches_v2: %w", err)
		}
		if _, err = tx.Exec(`DROP TABLE branches`); err != nil {
			return fmt.Errorf("drop old branches: %w", err)
		}
		if _, err = tx.Exec(`ALTER TABLE branches_v2 RENAME TO branches`); err != nil {
			return fmt.Errorf("rename branches_v2: %w", err)
		}
		return nil
	})
	mig.Register("handoff", 3, func(tx *store.TxStore) error {
		// git_branch 컬럼 추가 — hook fallback 조회용
		_, err := tx.Exec(`ALTER TABLE branches ADD COLUMN git_branch TEXT`)
		return err
	})
	mig.Register("handoff", 4, func(tx *store.TxStore) error {
		// status를 UPPER_SNAKE_CASE로 정규화
		_, err := tx.Exec(`UPDATE branches SET status = CASE status
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
		if _, err := tx.Exec(`ALTER TABLE branches RENAME TO active_branches`); err != nil {
			return fmt.Errorf("rename branches: %w", err)
		}

		// 2. branch_history 테이블 생성
		if _, err := tx.Exec(`CREATE TABLE branch_history (
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
		if _, err := tx.Exec(`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
			SELECT branch, workspace, git_branch, 'MERGED', summary, created_at
			FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
			return fmt.Errorf("migrate merge_notified: %w", err)
		}
		if _, err := tx.Exec(`DELETE FROM active_branches WHERE status = 'MERGE_NOTIFIED'`); err != nil {
			return fmt.Errorf("delete merge_notified: %w", err)
		}

		// 4. 나머지 전부 → branch_history (DROPPED) 이관
		if _, err := tx.Exec(`INSERT INTO branch_history (branch, workspace, git_branch, status, summary, started_at)
			SELECT branch, workspace, git_branch, 'DROPPED', summary, created_at
			FROM active_branches`); err != nil {
			return fmt.Errorf("migrate remaining: %w", err)
		}
		if _, err := tx.Exec(`DELETE FROM active_branches`); err != nil {
			return fmt.Errorf("delete remaining: %w", err)
		}

		// 5. git_branch UNIQUE 제약 추가 (테이블 재생성)
		if _, err := tx.Exec(`CREATE TABLE active_branches_v2 (
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
		if _, err := tx.Exec(`DROP TABLE active_branches`); err != nil {
			return fmt.Errorf("drop active_branches: %w", err)
		}
		if _, err := tx.Exec(`ALTER TABLE active_branches_v2 RENAME TO active_branches`); err != nil {
			return fmt.Errorf("rename active_branches_v2: %w", err)
		}

		// 6. 고아 branch_backlogs 정리
		if _, err := tx.Exec(`DELETE FROM branch_backlogs WHERE branch NOT IN (SELECT branch FROM active_branches)`); err != nil {
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
			if _, err := tx.Exec(q); err != nil {
				return fmt.Errorf("drop notification tables: %w", err)
			}
		}
		return nil
	})
}

// RegisterRoutes registers handoff action handlers.
func (m *Module) RegisterRoutes(reg daemon.RouteRegistrar) {
	reg.Handle("notify-start", m.handleNotifyStart)
	reg.Handle("notify-transition", m.handleNotifyTransition)
	reg.Handle("notify-merge", m.handleNotifyMerge)
	reg.Handle("notify-drop", m.handleNotifyDrop)
	reg.Handle("list-active", m.handleListActive)
	reg.Handle("backlog-check", m.handleBacklogCheck)
	reg.Handle("get-branch", m.handleGetBranch)
	reg.Handle("get-status", m.handleGetStatus)
	reg.Handle("resolve-branch", m.handleResolveBranch)
	reg.Handle("validate-commit", m.handleValidateCommit)
	reg.Handle("validate-merge-gate", m.handleValidateMergeGate)
	reg.Handle("validate-edit", m.handleValidateEdit)
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

func (m *Module) handleNotifyStart(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyStartParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.NotifyStart(p.Branch, p.Workspace, p.Summary, p.BranchName, p.BacklogIDs, p.Scopes, p.SkipDesign); err != nil {
		return nil, err
	}
	return map[string]string{"status": "started"}, nil
}

func (m *Module) handleNotifyTransition(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyTransitionParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.NotifyTransition(p.Branch, p.Workspace, p.Type, p.Summary); err != nil {
		return nil, err
	}
	return map[string]string{"status": "transitioned"}, nil
}

func (m *Module) handleBacklogCheck(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p backlogCheckParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	available, branch, err := m.manager.BacklogCheck(p.BacklogID)
	if err != nil {
		return nil, err
	}
	return map[string]any{"available": available, "branch": branch}, nil
}

func (m *Module) handleGetBranch(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p getBranchParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	b, err := m.manager.GetBranch(p.Branch)
	if err != nil {
		return nil, err
	}
	return map[string]any{"branch": b}, nil
}

func (m *Module) handleGetStatus(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p getStatusParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	status, err := m.manager.GetStatus(p.Branch)
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

func (m *Module) handleResolveBranch(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p resolveBranchParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	branch, err := m.manager.ResolveBranch(p.WorkspaceID, p.GitBranch)
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

func (m *Module) handleValidateCommit(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p validateCommitParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.ValidateCommit(p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

func (m *Module) handleValidateMergeGate(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p validateMergeGateParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.ValidateMergeGate(p.Branch); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

func (m *Module) handleValidateEdit(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p validateEditParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.ValidateEdit(p.Branch, p.FilePath); err != nil {
		return nil, err
	}
	return map[string]string{"status": "allowed"}, nil
}

// --- Merge / Drop / ListActive handlers ---

type notifyMergeParams struct {
	Branch    string `json:"branch"`
	Workspace string `json:"workspace"`
	Summary   string `json:"summary"`
}

func (m *Module) handleNotifyMerge(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyMergeParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.NotifyMerge(p.Branch, p.Workspace, p.Summary); err != nil {
		return nil, err
	}
	return map[string]string{"status": "merged"}, nil
}

type notifyDropParams struct {
	Branch    string `json:"branch"`
	Workspace string `json:"workspace"`
	Reason    string `json:"reason"`
}

func (m *Module) handleNotifyDrop(_ context.Context, params json.RawMessage, _ string) (any, error) {
	var p notifyDropParams
	if err := json.Unmarshal(params, &p); err != nil {
		return nil, fmt.Errorf("decode params: %w", err)
	}
	if err := m.manager.NotifyDrop(p.Branch, p.Workspace, p.Reason); err != nil {
		return nil, err
	}
	return map[string]string{"status": "dropped"}, nil
}

func (m *Module) handleListActive(_ context.Context, _ json.RawMessage, _ string) (any, error) {
	list, err := m.manager.ListActive()
	if err != nil {
		return nil, err
	}
	return map[string]any{"branches": list}, nil
}
