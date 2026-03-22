// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import (
	"fmt"
	"strings"
)

// --- Severity ---

const (
	SeverityCritical = "CRITICAL"
	SeverityMajor    = "MAJOR"
	SeverityMinor    = "MINOR"
)

var validSeverities = map[string]bool{
	SeverityCritical: true, SeverityMajor: true, SeverityMinor: true,
}

func ValidateSeverity(s string) error {
	if !validSeverities[s] {
		return fmt.Errorf("invalid severity %q: must be CRITICAL|MAJOR|MINOR", s)
	}
	return nil
}

// --- Timeframe ---

const (
	TimeframeNow      = "NOW"
	TimeframeInView   = "IN_VIEW"
	TimeframeDeferred = "DEFERRED"
)

var validTimeframes = map[string]bool{
	TimeframeNow: true, TimeframeInView: true, TimeframeDeferred: true,
	"": true, // 히스토리 import용
}

func ValidateTimeframe(s string) error {
	if !validTimeframes[s] {
		return fmt.Errorf("invalid timeframe %q: must be NOW|IN_VIEW|DEFERRED", s)
	}
	return nil
}

// --- Type ---

const (
	TypeBug        = "BUG"
	TypeDesignDebt = "DESIGN_DEBT"
	TypeDocs       = "DOCS"
	TypeInfra      = "INFRA"
	TypePerf       = "PERF"
	TypeSecurity   = "SECURITY"
	TypeTest       = "TEST"
)

var validTypes = map[string]bool{
	TypeBug: true, TypeDesignDebt: true, TypeDocs: true, TypeInfra: true,
	TypePerf: true, TypeSecurity: true, TypeTest: true,
}

func ValidateType(s string) error {
	if !validTypes[s] {
		return fmt.Errorf("invalid type %q: must be BUG|DESIGN_DEBT|DOCS|INFRA|PERF|SECURITY|TEST", s)
	}
	return nil
}

// --- Status ---

const (
	StatusOpen     = "OPEN"
	StatusFixing   = "FIXING"
	StatusResolved = "RESOLVED"
)

var validStatuses = map[string]bool{
	StatusOpen: true, StatusFixing: true, StatusResolved: true,
}

func ValidateStatus(s string) error {
	if !validStatuses[s] {
		return fmt.Errorf("invalid status %q: must be OPEN|FIXING|RESOLVED", s)
	}
	return nil
}

// --- Resolution ---

const (
	ResolutionFixed      = "FIXED"
	ResolutionDocumented = "DOCUMENTED"
	ResolutionWontfix    = "WONTFIX"
	ResolutionDuplicate  = "DUPLICATE"
	ResolutionSuperseded = "SUPERSEDED"
)

var validResolutions = map[string]bool{
	ResolutionFixed: true, ResolutionDocumented: true, ResolutionWontfix: true,
	ResolutionDuplicate: true, ResolutionSuperseded: true,
}

func ValidateResolution(s string) error {
	if !validResolutions[s] {
		return fmt.Errorf("invalid resolution %q: must be FIXED|DOCUMENTED|WONTFIX|DUPLICATE|SUPERSEDED", s)
	}
	return nil
}

// --- Scope ---

const (
	ScopeCore    = "CORE"
	ScopeShared  = "SHARED"
	ScopeGateway = "GATEWAY"
	ScopeAuthSvc = "AUTH_SVC"
	ScopeChatSvc = "CHAT_SVC"
	ScopeInfra   = "INFRA"
	ScopeCI      = "CI"
	ScopeDocs    = "DOCS"
	ScopeTools   = "TOOLS"
)

var validScopes = map[string]bool{
	ScopeCore: true, ScopeShared: true, ScopeGateway: true,
	ScopeAuthSvc: true, ScopeChatSvc: true, ScopeInfra: true,
	ScopeCI: true, ScopeDocs: true, ScopeTools: true,
}

// ValidateScope validates a comma-separated scope string.
// Each token is trimmed and checked individually.
func ValidateScope(s string) error {
	if s == "" {
		return fmt.Errorf("scope must not be empty")
	}
	for _, part := range strings.Split(s, ",") {
		token := strings.TrimSpace(part)
		if token == "" {
			continue
		}
		if !validScopes[token] {
			return fmt.Errorf("invalid scope %q: must be CORE|SHARED|GATEWAY|AUTH_SVC|CHAT_SVC|INFRA|CI|DOCS|TOOLS", token)
		}
	}
	return nil
}

// NormalizeScope converts legacy scope formats to UPPER_SNAKE_CASE.
// "core, shared" → "CORE, SHARED", "auth-svc" → "AUTH_SVC"
func NormalizeScope(s string) string {
	parts := strings.Split(s, ",")
	for i, p := range parts {
		p = strings.TrimSpace(p)
		p = strings.ToUpper(p)
		p = strings.ReplaceAll(p, "-", "_")
		parts[i] = p
	}
	return strings.Join(parts, ", ")
}

// NormalizeType converts legacy type formats to UPPER_SNAKE_CASE.
// "bug" → "BUG", "design-debt" → "DESIGN_DEBT"
func NormalizeType(s string) string {
	s = strings.ToUpper(s)
	s = strings.ReplaceAll(s, "-", "_")
	return s
}

// NormalizeStatus converts legacy status formats.
// "open" → "OPEN", "resolved" → "RESOLVED"
func NormalizeStatus(s string) string {
	return strings.ToUpper(s)
}

// NormalizeResolution cleans up resolution values.
// Handles corrupted data like "WONTFIX → **정정: FIXED (v0.5.10.2)**" → "FIXED"
func NormalizeResolution(s string) string {
	s = strings.TrimSpace(s)
	// 오염 데이터: "WONTFIX → ..." 패턴 → 마지막 유효 resolution 추출
	if strings.Contains(s, "→") || strings.Contains(s, "정정") {
		for _, res := range []string{"FIXED", "DOCUMENTED", "WONTFIX", "DUPLICATE", "SUPERSEDED"} {
			// 마지막으로 등장하는 유효 resolution을 사용
			if strings.Contains(strings.ToUpper(s), res) {
				// "정정: FIXED" 패턴이면 FIXED 사용
				if strings.Contains(s, "정정") {
					idx := strings.LastIndex(strings.ToUpper(s), res)
					if idx >= 0 {
						return res
					}
				}
			}
		}
	}
	return strings.ToUpper(s)
}
