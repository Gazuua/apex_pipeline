// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build !windows

package ipc

import (
	"fmt"
	"net"
	"os"
)

func listenPlatform(addr string) (net.Listener, error) {
	os.Remove(addr)
	ln, err := net.Listen("unix", addr)
	if err != nil {
		return nil, err
	}
	if chErr := os.Chmod(addr, 0o600); chErr != nil {
		ln.Close()
		return nil, fmt.Errorf("chmod socket: %w", chErr)
	}
	return ln, nil
}

func dialPlatform(addr string) (net.Conn, error) {
	return net.Dial("unix", addr)
}
