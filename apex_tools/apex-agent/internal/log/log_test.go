// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package log

import (
	"bytes"
	"strings"
	"testing"
)

func TestNew_WritesToBuffer(t *testing.T) {
	var buf bytes.Buffer
	logger := New(WithWriter(&buf))
	logger.Info("test message", "key", "value")

	out := buf.String()
	if !strings.Contains(out, "test message") {
		t.Errorf("log output = %q, want contains 'test message'", out)
	}
	if !strings.Contains(out, "key=value") && !strings.Contains(out, `"key":"value"`) {
		t.Errorf("log output = %q, want contains key/value", out)
	}
}

func TestNew_DefaultDoesNotPanic(t *testing.T) {
	logger := New()
	logger.Info("should not panic")
}
