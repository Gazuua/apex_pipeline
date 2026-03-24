// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"context"
	"fmt"
)

// ValidateCommit checks if a branch is registered for committing.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateCommit(ctx context.Context, branch string) error {
	status, err := m.GetStatus(ctx, branch)
	if err != nil {
		return err
	}
	if status == "" {
		ml.Warn("gate blocked", "op", "commit", "branch", branch, "reason", "not registered")
		return fmt.Errorf("차단: 핸드오프 미등록 상태에서 커밋 불가")
	}
	ml.Debug("gate check", "op", "commit", "branch", branch, "allowed", true)
	return nil
}

// ValidateMergeGate checks if there are FIXING backlogs before merge.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateMergeGate(ctx context.Context, branch string) error {
	fixingIDs, err := m.checkFixingBacklogs(ctx, branch)
	if err != nil {
		return fmt.Errorf("check fixing backlogs: %w", err)
	}
	if len(fixingIDs) > 0 {
		ml.Warn("gate blocked", "op", "merge", "branch", branch, "fixing", fixingIDs)
		return fmt.Errorf("차단: 미해결 백로그 %d건 (IDs: %v) — resolve 또는 release 후 재시도", len(fixingIDs), fixingIDs)
	}

	ml.Debug("gate check", "op", "merge", "branch", branch, "allowed", true)
	return nil
}

// ValidateEdit checks if a file edit is allowed based on branch status.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateEdit(ctx context.Context, branch, filePath string) error {
	status, err := m.GetStatus(ctx, branch)
	if err != nil {
		return err
	}

	// Not registered → block all edits
	if status == "" {
		ml.Warn("gate blocked", "op", "edit", "branch", branch, "reason", "not registered")
		return fmt.Errorf("차단: 핸드오프 미등록")
	}

	// Source file in non-implementing status → block
	if IsSourceFile(filePath) && !CanEditSource(status) {
		ml.Warn("gate blocked", "op", "edit", "branch", branch, "file", filePath, "status", status)
		switch status {
		case StatusStarted:
			return fmt.Errorf("차단: 설계 미완료(status=STARTED). 소스 파일 편집 불가")
		case StatusDesignNotified:
			return fmt.Errorf("차단: 구현 계획 미완료(status=DESIGN_NOTIFIED). 소스 파일 편집 불가")
		default:
			return fmt.Errorf("차단: status=%s에서 소스 파일 편집 불가", status)
		}
	}

	ml.Debug("gate check", "op", "edit", "branch", branch, "file", filePath, "allowed", true)
	return nil
}
