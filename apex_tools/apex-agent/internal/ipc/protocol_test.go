// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"bytes"
	"encoding/json"
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
