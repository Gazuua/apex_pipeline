// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package workflow

import (
	"fmt"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
)

// IPCFunc is the abstraction for notify IPC calls.
// CLI: sendHandoffRequest() wrapper.
// HTTP: direct Manager method call wrapper.
type IPCFunc func(action string, params map[string]any) (map[string]any, error)

// StartPipeline orchestrates the full notify-start workflow:
//  1. ValidateNewBranch(branchName)
//  2. ipcFn("notify-start", params) → DB TX
//  3. CreateAndPushBranch(branchName)
//  4. SyncImport(projectRoot, mgr) — non-fatal
func StartPipeline(branchName string, params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) (float64, error) {

	// Phase 1: git 사전 검증
	if err := ValidateNewBranch(projectRoot, branchName); err != nil {
		return 0, err
	}

	// Phase 2: DB TX
	result, err := ipcFn("notify-start", params)
	if err != nil {
		return 0, fmt.Errorf("notify-start 실패: %w", err)
	}
	notifID, _ := result["notification_id"].(float64)

	// Phase 3: git 브랜치 생성
	if err := CreateAndPushBranch(projectRoot, branchName); err != nil {
		return notifID, fmt.Errorf("브랜치 생성 실패: %w", err)
	}

	// Phase 4: backlog import (non-fatal)
	if mgr != nil {
		if n, syncErr := SyncImport(projectRoot, mgr); syncErr != nil {
			ml.Warn("착수 시 backlog import 실패 (작업 진행에 영향 없음)", "err", syncErr)
		} else if n > 0 {
			ml.Info("착수 시 backlog import 완료", "items", n)
		}
	}

	return notifID, nil
}

// MergePipeline orchestrates the full notify-merge workflow:
//  1. RebaseOnMain(projectRoot)
//  2. SyncImport(projectRoot, mgr) — fatal
//  3. SyncExport(projectRoot, mgr) — fatal
//  4. ipcFn("notify-merge", params) — fatal
//  5. CheckoutMain(projectRoot) — non-fatal
func MergePipeline(params map[string]any,
	projectRoot string, mgr *backlog.Manager, ipcFn IPCFunc) error {

	// Phase 1: rebase
	if msg, err := RebaseOnMain(projectRoot); err != nil {
		return err
	} else if msg != "" {
		fmt.Println(msg)
	}

	// Phase 2: import (rebase 후 최신 MD → DB)
	if mgr != nil {
		if _, err := SyncImport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog import 실패: %w", err)
		}
	}

	// Phase 3: export (DB → MD)
	if mgr != nil {
		if _, err := SyncExport(projectRoot, mgr); err != nil {
			return fmt.Errorf("머지 전 backlog export 실패: %w", err)
		}
	}

	// Phase 4: DB TX
	if _, err := ipcFn("notify-merge", params); err != nil {
		return fmt.Errorf("notify-merge 실패: %w", err)
	}

	// Phase 5: checkout main (non-fatal)
	if err := CheckoutMain(projectRoot); err != nil {
		ml.Warn("git checkout main 실패", "err", err)
	}

	return nil
}

// DropPipeline orchestrates the full notify-drop workflow:
//  1. ipcFn("notify-drop", params) → DB TX
//  2. CheckoutMain(projectRoot) — non-fatal
func DropPipeline(params map[string]any,
	projectRoot string, ipcFn IPCFunc) error {

	if _, err := ipcFn("notify-drop", params); err != nil {
		return fmt.Errorf("notify-drop 실패: %w", err)
	}

	if err := CheckoutMain(projectRoot); err != nil {
		ml.Warn("git checkout main 실패", "err", err)
	}

	return nil
}
