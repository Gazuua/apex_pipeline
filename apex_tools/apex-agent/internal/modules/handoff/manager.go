// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"

// Manager provides handoff business logic.
// Implementation will be fleshed out in Task 2.
type Manager struct {
	store *store.Store
}

// NewManager creates a new Manager backed by the given store.
func NewManager(s *store.Store) *Manager {
	return &Manager{store: s}
}
