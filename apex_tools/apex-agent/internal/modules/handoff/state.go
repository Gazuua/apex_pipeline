// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import "fmt"

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
func IsSourceFile(path string) bool {
	// Currently checks C++ and Go source extensions.
	// The handoff system gates source files differently from docs.
	suffixes := []string{".cpp", ".hpp", ".h", ".c", ".cc", ".cxx", ".hxx", ".go"}
	for _, s := range suffixes {
		if len(path) > len(s) && path[len(path)-len(s):] == s {
			return true
		}
	}
	return false
}
