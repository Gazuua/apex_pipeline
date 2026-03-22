// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import (
	"fmt"
	"path/filepath"
	"strings"
)

// Valid statuses
const (
	StatusStarted        = "STARTED"
	StatusDesignNotified = "DESIGN_NOTIFIED"
	StatusImplementing   = "IMPLEMENTING"
	StatusMergeNotified  = "MERGE_NOTIFIED"
)

// Valid notification types
const (
	TypeStart  = "start"
	TypeDesign = "design"
	TypePlan   = "plan"
	TypeMerge  = "merge"
)

// NextStatus returns the new status after applying a notification type.
// Returns error if the transition is invalid.
func NextStatus(current, notifyType string) (string, error) {
	switch {
	case current == StatusStarted && notifyType == TypeDesign:
		return StatusDesignNotified, nil
	case current == StatusDesignNotified && notifyType == TypePlan:
		return StatusImplementing, nil
	case current == StatusImplementing && notifyType == TypeMerge:
		return StatusMergeNotified, nil
	default:
		return "", fmt.Errorf("invalid transition: %s + %s", current, notifyType)
	}
}

// CanEditSource returns whether source files (.cpp, .hpp, .h, .go) can be edited in the given status.
func CanEditSource(status string) bool {
	return status == StatusImplementing || status == StatusMergeNotified
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
