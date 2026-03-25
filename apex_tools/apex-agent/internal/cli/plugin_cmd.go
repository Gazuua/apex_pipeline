// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/plugin"
)

func pluginCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "plugin",
		Short: "플러그인 관리",
	}
	cmd.AddCommand(pluginSetupCmd())
	return cmd
}

// ensureDaemon checks if the daemon is running and starts it if not.
// Respects maintenance lock written by "daemon stop" — skips auto-start
// during binary upgrade to prevent restarting the old binary.
func ensureDaemon() {
	conn, err := ipc.Dial(platform.SocketPath())
	if err == nil {
		conn.Close()
		return // 이미 실행 중
	}

	// Maintenance lock present → daemon was intentionally stopped (e.g., binary upgrade).
	if _, err := os.Stat(platform.MaintenanceFilePath()); err == nil {
		return
	}

	if err := platform.EnsureDataDir(); err != nil {
		fmt.Fprintf(os.Stderr, "[apex-agent] data dir error: %v\n", err)
		return
	}
	pid, err := startDaemonProcess()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[apex-agent] %v\n", err)
		return
	}
	fmt.Fprintf(os.Stderr, "[apex-agent] daemon auto-started (pid %d)\n", pid)
}

func pluginSetupCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "setup",
		Short: "auto-review 플러그인 등록",
		Long:  "SessionStart 훅에서 호출 — apex-auto-review 플러그인을 ~/.claude에 idempotent하게 등록합니다.",
		RunE: func(cmd *cobra.Command, args []string) error {
			// 데몬 자동 기동 — 미실행 시 시작
			ensureDaemon()

			cwd, err := os.Getwd()
			if err != nil {
				return fmt.Errorf("getwd: %w", err)
			}
			return plugin.Setup(cwd)
		},
	}
}
