// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"
	"os/exec"
	"time"

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
func ensureDaemon() {
	conn, err := ipc.Dial(platform.SocketPath())
	if err == nil {
		conn.Close()
		return // 이미 실행 중
	}

	// 데몬 시작 — daemonStartCmd와 동일 로직 (인라인)
	if err := platform.EnsureDataDir(); err != nil {
		fmt.Fprintf(os.Stderr, "[apex-agent] data dir error: %v\n", err)
		return
	}
	exe, _ := os.Executable()
	child := exec.Command(exe, "daemon", "run")
	detachProcess(child)
	if err := child.Start(); err != nil {
		fmt.Fprintf(os.Stderr, "[apex-agent] daemon start failed: %v\n", err)
		return
	}
	// 소켓 준비 대기 (최대 5초)
	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		conn, err := ipc.Dial(platform.SocketPath())
		if err == nil {
			conn.Close()
			fmt.Fprintf(os.Stderr, "[apex-agent] daemon auto-started (pid %d)\n", child.Process.Pid)
			return
		}
	}
	fmt.Fprintf(os.Stderr, "[apex-agent] daemon start timeout\n")
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
