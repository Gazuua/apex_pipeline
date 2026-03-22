// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package ipc

import "net"

// Listen creates a platform-specific IPC listener.
// Windows: Named Pipe, Unix: Unix Domain Socket.
func Listen(addr string) (net.Listener, error) {
	return listenPlatform(addr)
}

// Dial connects to the platform-specific IPC endpoint.
func Dial(addr string) (net.Conn, error) {
	return dialPlatform(addr)
}
