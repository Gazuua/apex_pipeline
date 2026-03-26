// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"bytes"
	"io"
	"sync"
)

// Terminal is the interface for a pseudo-terminal process.
// ConPTY implements this on Windows; Unix uses a stub.
type Terminal interface {
	io.ReadWriteCloser
	// Resize changes the terminal dimensions.
	Resize(cols, rows int) error
	// Pid returns the process ID of the child process.
	Pid() int
}

// RingBuffer stores the last N lines of terminal output for replay on reconnect.
type RingBuffer struct {
	mu       sync.Mutex
	lines    []string
	maxLines int
	pos      int
	full     bool
	partial  string // incomplete line (no trailing newline yet)
}

// NewRingBuffer creates a ring buffer that keeps at most maxLines lines.
func NewRingBuffer(maxLines int) *RingBuffer {
	return &RingBuffer{
		lines:    make([]string, maxLines),
		maxLines: maxLines,
	}
}

// Write appends data to the buffer, splitting on newlines.
func (rb *RingBuffer) Write(data []byte) {
	rb.mu.Lock()
	defer rb.mu.Unlock()

	buf := rb.partial + string(data)
	rb.partial = ""

	for {
		idx := bytes.IndexByte([]byte(buf), '\n')
		if idx < 0 {
			rb.partial = buf
			return
		}
		line := buf[:idx+1]
		buf = buf[idx+1:]

		rb.lines[rb.pos] = line
		rb.pos = (rb.pos + 1) % rb.maxLines
		if rb.pos == 0 {
			rb.full = true
		}
	}
}

// Lines returns all stored lines in chronological order.
func (rb *RingBuffer) Lines() []string {
	rb.mu.Lock()
	defer rb.mu.Unlock()

	if !rb.full {
		result := make([]string, rb.pos)
		copy(result, rb.lines[:rb.pos])
		return result
	}
	result := make([]string, rb.maxLines)
	copy(result, rb.lines[rb.pos:])
	copy(result[rb.maxLines-rb.pos:], rb.lines[:rb.pos])
	return result
}

// Snapshot returns all stored lines concatenated as a single byte slice.
func (rb *RingBuffer) Snapshot() []byte {
	lines := rb.Lines()
	var buf bytes.Buffer
	for _, l := range lines {
		buf.WriteString(l)
	}
	return buf.Bytes()
}
