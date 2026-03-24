// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build windows

package platform

import "golang.org/x/sys/windows"

func IsProcessAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, uint32(pid))
	if err != nil {
		return false
	}
	_ = windows.CloseHandle(h)
	return true
}
