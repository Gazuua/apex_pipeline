// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
)

// Request is the JSON message sent from CLI to daemon.
type Request struct {
	Module    string          `json:"module"`
	Action    string          `json:"action"`
	Params    json.RawMessage `json:"params,omitempty"`
	Workspace string          `json:"workspace"`
}

// Response is the JSON message sent from daemon to CLI.
// This is the ONLY place Response is defined — daemon/router.go returns (any, error).
type Response struct {
	OK    bool            `json:"ok"`
	Data  json.RawMessage `json:"data,omitempty"`
	Error string          `json:"error,omitempty"`
}

const maxMessageSize = 4 * 1024 * 1024 // 4MB safety limit

// WriteMessage serializes v to JSON and writes it with a 4-byte big-endian length prefix.
func WriteMessage(w io.Writer, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}
	length := uint32(len(data))
	if err := binary.Write(w, binary.BigEndian, length); err != nil {
		return fmt.Errorf("write length: %w", err)
	}
	if _, err := w.Write(data); err != nil {
		return fmt.Errorf("write payload: %w", err)
	}
	return nil
}

// ReadMessage reads a length-prefixed JSON message and unmarshals into v.
func ReadMessage(r io.Reader, v any) error {
	var length uint32
	if err := binary.Read(r, binary.BigEndian, &length); err != nil {
		return fmt.Errorf("read length: %w", err)
	}
	if length > maxMessageSize {
		return fmt.Errorf("message too large: %d bytes", length)
	}
	data := make([]byte, length)
	if _, err := io.ReadFull(r, data); err != nil {
		return fmt.Errorf("read payload: %w", err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		return fmt.Errorf("unmarshal: %w", err)
	}
	return nil
}
