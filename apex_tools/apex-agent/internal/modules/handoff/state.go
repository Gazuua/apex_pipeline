// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"fmt"
	"path/filepath"
	"strings"
)

// Valid statuses for active_branches.
const (
	StatusStarted        = "STARTED"
	StatusDesignNotified = "DESIGN_NOTIFIED"
	StatusImplementing   = "IMPLEMENTING"
)

// History statuses (branch_history only — never in active_branches).
const (
	HistoryMerged  = "MERGED"
	HistoryDropped = "DROPPED"
)

// Valid notification types.
// 의도적 lowercase 유지: CLI 인자("start", "design", "merge")와 직접 매핑되며
// DB에도 이 값으로 저장됨. 다른 열거형(backlog status/severity 등)은 UPPER_SNAKE_CASE이지만,
// notification type은 CLI 사용성과 기존 DB 호환을 위해 lowercase를 유지한다.
const (
	TypeStart  = "start"
	TypeDesign = "design"
	TypePlan   = "plan"
	TypeMerge  = "merge"
	TypeDrop   = "drop"
)

// NextStatus returns the new status after applying a notification type.
// Returns error if the transition is invalid.
// Note: merge/drop are handled by dedicated functions (NotifyMerge/NotifyDrop),
// not through NextStatus.
func NextStatus(current, notifyType string) (string, error) {
	switch {
	case current == StatusStarted && notifyType == TypeDesign:
		return StatusDesignNotified, nil
	case current == StatusDesignNotified && notifyType == TypePlan:
		return StatusImplementing, nil
	default:
		return "", fmt.Errorf("invalid transition: %s + %s", current, notifyType)
	}
}

// CanEditSource returns whether source files (.cpp, .hpp, .h, .go) can be edited in the given status.
func CanEditSource(status string) bool {
	return status == StatusImplementing
}

// CanEditAny returns whether any file can be edited in the given status.
// Returns false only when not registered (empty status).
func CanEditAny(status string) bool {
	return status != ""
}

// IsSourceFile checks if a file path is a source code file.
// Returns false for bare extensions (e.g., ".cpp" with no filename).
func IsSourceFile(path string) bool {
	base := filepath.Base(path)
	ext := strings.ToLower(filepath.Ext(base))
	if ext == "" || base == ext {
		return false // no extension, or bare extension without filename
	}
	switch ext {
	case ".cpp", ".hpp", ".h", ".c", ".cc", ".cxx", ".hxx", ".go":
		return true
	}
	return false
}
