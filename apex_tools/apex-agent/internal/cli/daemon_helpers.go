// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"
	"os/exec"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// startDaemonProcess spawns the daemon in the background and waits for the
// IPC socket to become ready (up to 5 seconds). Returns the child PID on
// success or an error if the daemon fails to start within the timeout.
func startDaemonProcess() (int, error) {
	exe, _ := os.Executable()
	child := exec.Command(exe, "daemon", "run")
	detachProcess(child)
	if err := child.Start(); err != nil {
		return 0, fmt.Errorf("start daemon: %w", err)
	}

	addr := platform.SocketPath()
	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		conn, err := ipc.Dial(addr)
		if err == nil {
			conn.Close()
			return child.Process.Pid, nil
		}
	}
	return 0, fmt.Errorf("daemon failed to start within 5 seconds")
}
