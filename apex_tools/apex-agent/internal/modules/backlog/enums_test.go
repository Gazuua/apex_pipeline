// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package backlog

import "testing"

// ── ValidateSeverity ──

func TestValidateSeverity(t *testing.T) {
	tests := []struct {
		input   string
		wantErr bool
	}{
		{"CRITICAL", false},
		{"MAJOR", false},
		{"MINOR", false},
		{"critical", true},
		{"high", true},
		{"", true},
		{"LOW", true},
		{"Major", true},
	}
	for _, tc := range tests {
		t.Run(tc.input, func(t *testing.T) {
			err := ValidateSeverity(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateSeverity(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── ValidateTimeframe ──

func TestValidateTimeframe(t *testing.T) {
	tests := []struct {
		input   string
		wantErr bool
	}{
		{"NOW", false},
		{"IN_VIEW", false},
		{"DEFERRED", false},
		{"", false}, // 히스토리 import용 허용
		{"now", true},
		{"in_view", true},
		{"IMMEDIATE", true},
		{"In_View", true},
	}
	for _, tc := range tests {
		name := tc.input
		if name == "" {
			name = "(empty)"
		}
		t.Run(name, func(t *testing.T) {
			err := ValidateTimeframe(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateTimeframe(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── ValidateType ──

func TestValidateType(t *testing.T) {
	tests := []struct {
		input   string
		wantErr bool
	}{
		{"BUG", false},
		{"DESIGN_DEBT", false},
		{"TEST", false},
		{"DOCS", false},
		{"PERF", false},
		{"SECURITY", false},
		{"INFRA", false},
		{"bug", true},
		{"design-debt", true},
		{"", true},
		{"FEATURE", true},
	}
	for _, tc := range tests {
		name := tc.input
		if name == "" {
			name = "(empty)"
		}
		t.Run(name, func(t *testing.T) {
			err := ValidateType(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateType(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── ValidateScope ──

func TestValidateScope(t *testing.T) {
	tests := []struct {
		name    string
		input   string
		wantErr bool
	}{
		{"single CORE", "CORE", false},
		{"single SHARED", "SHARED", false},
		{"single GATEWAY", "GATEWAY", false},
		{"single AUTH_SVC", "AUTH_SVC", false},
		{"single CHAT_SVC", "CHAT_SVC", false},
		{"single INFRA", "INFRA", false},
		{"single CI", "CI", false},
		{"single DOCS", "DOCS", false},
		{"single TOOLS", "TOOLS", false},
		{"multi CORE,SHARED", "CORE,SHARED", false},
		{"multi with spaces", "CORE, SHARED", false},
		{"multi three scopes", "CORE, SHARED, INFRA", false},
		{"empty", "", true},
		{"lowercase", "core", true},
		{"invalid scope", "FRONTEND", true},
		{"mixed valid and invalid", "CORE,FRONTEND", true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := ValidateScope(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateScope(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── ValidateStatus ──

func TestValidateStatus(t *testing.T) {
	tests := []struct {
		input   string
		wantErr bool
	}{
		{"OPEN", false},
		{"FIXING", false},
		{"RESOLVED", false},
		{"open", true},
		{"", true},
		{"CLOSED", true},
		{"Fixing", true},
	}
	for _, tc := range tests {
		name := tc.input
		if name == "" {
			name = "(empty)"
		}
		t.Run(name, func(t *testing.T) {
			err := ValidateStatus(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateStatus(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── ValidateResolution ──

func TestValidateResolution(t *testing.T) {
	tests := []struct {
		input   string
		wantErr bool
	}{
		{"FIXED", false},
		{"DOCUMENTED", false},
		{"WONTFIX", false},
		{"DUPLICATE", false},
		{"SUPERSEDED", false},
		{"fixed", true},
		{"", true},
		{"INVALID", true},
		{"Won'tFix", true},
	}
	for _, tc := range tests {
		name := tc.input
		if name == "" {
			name = "(empty)"
		}
		t.Run(name, func(t *testing.T) {
			err := ValidateResolution(tc.input)
			if (err != nil) != tc.wantErr {
				t.Errorf("ValidateResolution(%q) error = %v, wantErr %v", tc.input, err, tc.wantErr)
			}
		})
	}
}

// ── NormalizeScope ──

func TestNormalizeScope(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{"lowercase to upper", "core", "CORE"},
		{"mixed case", "Core", "CORE"},
		{"multi scope lowercase", "core,shared", "CORE, SHARED"},
		{"multi scope with spaces", "core , shared", "CORE, SHARED"},
		{"hyphen to underscore", "auth-svc", "AUTH_SVC"},
		{"already uppercase", "CORE", "CORE"},
		{"multi sorted input", "SHARED,CORE", "SHARED, CORE"},
		{"complex multi", "auth-svc, chat-svc", "AUTH_SVC, CHAT_SVC"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := NormalizeScope(tc.input)
			if got != tc.want {
				t.Errorf("NormalizeScope(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

// ── NormalizeType ──

func TestNormalizeType(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{"lowercase bug", "bug", "BUG"},
		{"hyphenated design-debt", "design-debt", "DESIGN_DEBT"},
		{"already upper", "BUG", "BUG"},
		{"mixed case", "Design-Debt", "DESIGN_DEBT"},
		{"lowercase perf", "perf", "PERF"},
		{"lowercase security", "security", "SECURITY"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := NormalizeType(tc.input)
			if got != tc.want {
				t.Errorf("NormalizeType(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

// ── NormalizeStatus ──

func TestNormalizeStatus(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{"lowercase open", "open", "OPEN"},
		{"lowercase fixing", "fixing", "FIXING"},
		{"lowercase resolved", "resolved", "RESOLVED"},
		{"already upper", "OPEN", "OPEN"},
		{"mixed case", "Resolved", "RESOLVED"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := NormalizeStatus(tc.input)
			if got != tc.want {
				t.Errorf("NormalizeStatus(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}

// ── NormalizeResolution ──

func TestNormalizeResolution(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  string
	}{
		{"simple uppercase", "FIXED", "FIXED"},
		{"simple lowercase", "fixed", "FIXED"},
		{"documented", "documented", "DOCUMENTED"},
		{"arrow correction", "WONTFIX → DOCUMENTED", "DOCUMENTED"},
		{"korean correction prefix", "정정: FIXED", "FIXED"},
		{"complex corrupted", "WONTFIX → **정정: FIXED (v0.5)**", "FIXED"},
		{"arrow without 정정", "WONTFIX → SUPERSEDED", "SUPERSEDED"},
		{"already upper WONTFIX", "WONTFIX", "WONTFIX"},
		{"duplicate", "DUPLICATE", "DUPLICATE"},
		{"superseded", "SUPERSEDED", "SUPERSEDED"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := NormalizeResolution(tc.input)
			if got != tc.want {
				t.Errorf("NormalizeResolution(%q) = %q, want %q", tc.input, got, tc.want)
			}
		})
	}
}
