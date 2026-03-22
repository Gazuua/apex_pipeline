// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build windows

package ipc

import (
	"net"
	"time"

	"github.com/Microsoft/go-winio"
)

func listenPlatform(addr string) (net.Listener, error) {
	return winio.ListenPipe(addr, &winio.PipeConfig{
		SecurityDescriptor: "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
		MessageMode:        false,
		InputBufferSize:    64 * 1024,
		OutputBufferSize:   64 * 1024,
	})
}

func dialPlatform(addr string) (net.Conn, error) {
	return winio.DialPipe(addr, (*time.Duration)(nil))
}
