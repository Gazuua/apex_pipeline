// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import "fmt"

// ValidateCommit checks if a branch is registered for committing.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateCommit(branch string) error {
	status, err := m.GetStatus(branch)
	if err != nil {
		return err
	}
	if status == "" {
		return fmt.Errorf("차단: 핸드오프 미등록 상태에서 커밋 불가")
	}
	return nil
}

// ValidateMergeGate checks if all notifications are acked before merge.
// Returns nil if allowed, error if blocked.
func (m *Manager) ValidateMergeGate(branch string) error {
	notifications, err := m.CheckNotifications(branch)
	if err != nil {
		return err
	}
	if len(notifications) > 0 {
		return fmt.Errorf("차단: 미ack 알림 %d건. 먼저 ack 처리 후 머지 재시도", len(notifications))
	}
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
		return fmt.Errorf("차단: 핸드오프 미등록")
	}

	// Source file in non-implementing status → block
	if IsSourceFile(filePath) && !CanEditSource(status) {
		switch status {
		case StatusStarted:
			return fmt.Errorf("차단: 설계 미완료(status=started). 소스 파일 편집 불가")
		case StatusDesignNotified:
			return fmt.Errorf("차단: 구현 계획 미완료(status=design-notified). 소스 파일 편집 불가")
		default:
			return fmt.Errorf("차단: status=%s에서 소스 파일 편집 불가", status)
		}
	}

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
