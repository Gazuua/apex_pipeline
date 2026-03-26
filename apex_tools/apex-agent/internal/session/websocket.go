// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package session

import (
	"net/http"

	"github.com/gorilla/websocket"
)

var wsUpgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	CheckOrigin:     func(r *http.Request) bool { return true }, // localhost only
}

// HandleWebSocket upgrades an HTTP connection to WebSocket and bridges it
// to a terminal session's I/O. Binary frames carry raw terminal bytes.
func HandleWebSocket(mgr *Manager, w http.ResponseWriter, r *http.Request, workspaceID string) {
	info := mgr.Get(workspaceID)
	if info == nil {
		http.Error(w, "no active session for "+workspaceID, http.StatusNotFound)
		return
	}

	conn, err := wsUpgrader.Upgrade(w, r, nil)
	if err != nil {
		ml.Warn("websocket upgrade failed", "workspace", workspaceID, "err", err)
		return
	}
	defer conn.Close()

	// Replay buffered output on connect.
	snap := info.ring.Snapshot()
	if len(snap) > 0 {
		if err := conn.WriteMessage(websocket.BinaryMessage, snap); err != nil {
			return
		}
	}

	// Subscribe to live output.
	ch, unsub := info.Subscribe()
	defer unsub()

	// Write pump: terminal output → WebSocket.
	done := make(chan struct{})
	go func() {
		defer close(done)
		for data := range ch {
			if err := conn.WriteMessage(websocket.BinaryMessage, data); err != nil {
				return
			}
		}
	}()

	// Read pump: WebSocket → terminal stdin.
	for {
		_, msg, err := conn.ReadMessage()
		if err != nil {
			break
		}
		if err := info.SendInput(msg); err != nil {
			ml.Debug("stdin write failed", "workspace", workspaceID, "err", err)
			break
		}
	}

	unsub()
	<-done
}
