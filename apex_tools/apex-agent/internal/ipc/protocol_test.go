// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"strings"
	"testing"
)

func TestWriteRead_Roundtrip(t *testing.T) {
	var buf bytes.Buffer

	req := &Request{
		Module:    "handoff",
		Action:    "notify",
		Params:    json.RawMessage(`{"type":"start"}`),
		Workspace: "branch_02",
	}

	if err := WriteMessage(&buf, req); err != nil {
		t.Fatal(err)
	}

	var got Request
	if err := ReadMessage(&buf, &got); err != nil {
		t.Fatal(err)
	}

	if got.Module != "handoff" || got.Action != "notify" || got.Workspace != "branch_02" {
		t.Errorf("roundtrip mismatch: %+v", got)
	}
}

func TestWriteRead_Response(t *testing.T) {
	var buf bytes.Buffer

	resp := &Response{
		OK:   true,
		Data: json.RawMessage(`{"id":30}`),
	}

	WriteMessage(&buf, resp)

	var got Response
	ReadMessage(&buf, &got)

	if !got.OK {
		t.Error("OK = false, want true")
	}
}

func TestReadMessage_Empty(t *testing.T) {
	var buf bytes.Buffer
	var req Request
	err := ReadMessage(&buf, &req)
	if err == nil {
		t.Error("expected error on empty buffer")
	}
}

func TestReadMessage_MaxSizeExceeded(t *testing.T) {
	var buf bytes.Buffer

	// Write a length prefix of 4MB+1 (exceeds maxMessageSize)
	oversize := uint32(4*1024*1024 + 1)
	if err := binary.Write(&buf, binary.BigEndian, oversize); err != nil {
		t.Fatalf("write length prefix: %v", err)
	}

	var req Request
	err := ReadMessage(&buf, &req)
	if err == nil {
		t.Fatal("expected error for oversized message, got nil")
	}
	if !strings.Contains(err.Error(), "message too large") {
		t.Errorf("expected 'message too large' error, got: %v", err)
	}
}
