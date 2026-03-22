// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

//go:build windows

package cli

import (
	"os/exec"
	"syscall"
)

func detachProcess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: syscall.CREATE_NEW_PROCESS_GROUP,
	}
}
