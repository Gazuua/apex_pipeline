// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"bytes"
	"strings"
	"testing"
)

func TestDebug_WritesToBuffer(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	Debug("test message", "key", "value")

	out := buf.String()
	if !strings.Contains(out, "test message") {
		t.Errorf("output = %q, want contains 'test message'", out)
	}
}

func TestAudit_HasAuditTag(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	Audit("lock acquired", "channel", "build")

	out := buf.String()
	if !strings.Contains(out, "audit=true") {
		t.Errorf("audit log missing audit=true tag: %q", out)
	}
}

func TestWithModule_AddsModuleField(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "debug", Writer: &buf})
	ml := WithModule("handoff")
	ml.Info("state changed")

	out := buf.String()
	if !strings.Contains(out, "module=handoff") {
		t.Errorf("module log missing module field: %q", out)
	}
}

func TestLevelFilter_InfoHidesDebug(t *testing.T) {
	var buf bytes.Buffer
	Init(LogConfig{Level: "info", Writer: &buf})
	Debug("should be hidden")
	Info("should be visible")

	out := buf.String()
	if strings.Contains(out, "should be hidden") {
		t.Error("debug message should be filtered at info level")
	}
	if !strings.Contains(out, "should be visible") {
		t.Error("info message should be visible")
	}
}
