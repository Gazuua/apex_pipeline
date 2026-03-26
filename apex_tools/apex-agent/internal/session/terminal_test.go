// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"testing"
)

func TestRingBuffer_Basic(t *testing.T) {
	rb := NewRingBuffer(3)
	rb.Write([]byte("line1\nline2\nline3\n"))

	lines := rb.Lines()
	if len(lines) != 3 {
		t.Fatalf("expected 3 lines, got %d", len(lines))
	}
	if lines[0] != "line1\n" || lines[1] != "line2\n" || lines[2] != "line3\n" {
		t.Errorf("unexpected lines: %v", lines)
	}
}

func TestRingBuffer_Overflow(t *testing.T) {
	rb := NewRingBuffer(2)
	rb.Write([]byte("a\nb\nc\n"))

	lines := rb.Lines()
	if len(lines) != 2 {
		t.Fatalf("expected 2 lines, got %d", len(lines))
	}
	if lines[0] != "b\n" || lines[1] != "c\n" {
		t.Errorf("expected [b\\n, c\\n], got %v", lines)
	}
}

func TestRingBuffer_PartialLine(t *testing.T) {
	rb := NewRingBuffer(10)
	rb.Write([]byte("partial"))
	rb.Write([]byte(" line\nfull\n"))

	lines := rb.Lines()
	if len(lines) != 2 {
		t.Fatalf("expected 2 lines, got %d", len(lines))
	}
	if lines[0] != "partial line\n" {
		t.Errorf("expected merged partial, got %q", lines[0])
	}
}

func TestRingBuffer_Snapshot(t *testing.T) {
	rb := NewRingBuffer(5)
	rb.Write([]byte("hello\nworld\n"))

	snap := rb.Snapshot()
	if string(snap) != "hello\nworld\n" {
		t.Errorf("snapshot = %q, want %q", snap, "hello\nworld\n")
	}
}
