// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build !windows

package ipc

import (
	"net"
	"os"
)

func listenPlatform(addr string) (net.Listener, error) {
	os.Remove(addr)
	return net.Listen("unix", addr)
}

func dialPlatform(addr string) (net.Conn, error) {
	return net.Dial("unix", addr)
}
