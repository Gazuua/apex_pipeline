// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"fmt"
)

// ValidateCommit checks if a branch is registered for committing.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateCommit(branch string) error {
	status, err := m.GetStatus(branch)
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

// ValidateMergeGate checks if all notifications are acked before merge.
// Also blocks if there are FIXING backlogs attached to this branch.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateMergeGate(branch string) error {
	notifications, err := m.CheckNotifications(branch)
	if err != nil {
		return err
	}
	if len(notifications) > 0 {
		ml.Warn("gate blocked", "op", "merge", "branch", branch, "unacked", len(notifications))
		return fmt.Errorf("차단: 미ack 알림 %d건. 먼저 ack 처리 후 머지 재시도", len(notifications))
	}

	// FIXING 백로그 체크 (공용 함수 사용)
	fixingIDs, err := m.checkFixingBacklogs(branch)
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
func (m *Manager) ValidateEdit(branch, filePath string) error {
	status, err := m.GetStatus(branch)
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

// ProbeNotifications checks for unacked notifications and returns a formatted message.
// Returns empty string if no new notifications.
func (m *Manager) ProbeNotifications(branch string) (string, error) {
	notifications, err := m.CheckNotifications(branch)
	if err != nil {
		return "", err
	}
	if len(notifications) == 0 {
		return "", nil
	}

	msg := "─── 핸드오프 알림 ───\n"
	for _, n := range notifications {
		msg += fmt.Sprintf("#%d [%s] %s: %s\n", n.ID, n.Type, n.Branch, n.Summary)
	}
	msg += "────────────────────"
	return msg, nil
}
