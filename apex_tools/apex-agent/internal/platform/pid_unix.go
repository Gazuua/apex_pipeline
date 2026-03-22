// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build !windows

package platform

import "syscall"

func IsProcessAlive(pid int) bool {
	err := syscall.Kill(pid, 0)
	return err == nil
}
