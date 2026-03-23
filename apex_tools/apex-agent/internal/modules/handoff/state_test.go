// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package handoff

import "testing"

func TestNextStatus_ValidTransitions(t *testing.T) {
	tests := []struct {
		current    string
		notifyType string
		want       string
	}{
		{StatusStarted, TypeDesign, StatusDesignNotified},
		{StatusDesignNotified, TypePlan, StatusImplementing},
	}

	for _, tt := range tests {
		got, err := NextStatus(tt.current, tt.notifyType)
		if err != nil {
			t.Errorf("NextStatus(%s, %s) error: %v", tt.current, tt.notifyType, err)
		}
		if got != tt.want {
			t.Errorf("NextStatus(%s, %s) = %s, want %s", tt.current, tt.notifyType, got, tt.want)
		}
	}
}

func TestNextStatus_InvalidTransitions(t *testing.T) {
	tests := []struct {
		current    string
		notifyType string
	}{
		{StatusStarted, TypePlan},          // can't skip design→plan
		{StatusStarted, TypeMerge},         // merge is handled by NotifyMerge, not NextStatus
		{StatusDesignNotified, TypeDesign}, // already design-notified
		{StatusDesignNotified, TypeMerge},  // must go through plan
		{StatusImplementing, TypeDesign},   // can't go back
		{StatusImplementing, TypeMerge},    // merge is handled by NotifyMerge
	}

	for _, tt := range tests {
		_, err := NextStatus(tt.current, tt.notifyType)
		if err == nil {
			t.Errorf("NextStatus(%s, %s) should error", tt.current, tt.notifyType)
		}
	}
}

func TestCanEditSource(t *testing.T) {
	tests := []struct {
		status string
		want   bool
	}{
		{StatusStarted, false},
		{StatusDesignNotified, false},
		{StatusImplementing, true},
		{"", false},
	}

	for _, tt := range tests {
		got := CanEditSource(tt.status)
		if got != tt.want {
			t.Errorf("CanEditSource(%s) = %v, want %v", tt.status, got, tt.want)
		}
	}
}

func TestCanEditAny(t *testing.T) {
	if CanEditAny("") {
		t.Error("CanEditAny('') should be false")
	}
	if !CanEditAny(StatusStarted) {
		t.Error("CanEditAny('started') should be true")
	}
}

func TestIsSourceFile(t *testing.T) {
	tests := []struct {
		path string
		want bool
	}{
		{"server.cpp", true},
		{"header.hpp", true},
		{"main.go", true},
		{"README.md", false},
		{"docs/plan.md", false},
		{"config.toml", false},
		{".cpp", false}, // too short — just extension
	}

	for _, tt := range tests {
		got := IsSourceFile(tt.path)
		if got != tt.want {
			t.Errorf("IsSourceFile(%s) = %v, want %v", tt.path, got, tt.want)
		}
	}
}
