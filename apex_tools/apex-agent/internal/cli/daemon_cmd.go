// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

func daemonCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "daemon",
		Short: "데몬 관리",
	}
	cmd.AddCommand(daemonRunCmd())
	cmd.AddCommand(daemonStartCmd())
	cmd.AddCommand(daemonStopCmd())
	cmd.AddCommand(daemonStatusCmd())
	return cmd
}

func daemonRunCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "run",
		Short: "데몬 포그라운드 실행 (디버깅용)",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := platform.EnsureDataDir(); err != nil {
				return err
			}
			cfg := daemon.Config{
				DBPath:      platform.DBPath(),
				PIDFilePath: platform.PIDFilePath(),
				SocketAddr:  platform.SocketPath(),
				IdleTimeout: 30 * time.Minute,
			}
			d, err := daemon.New(cfg)
			if err != nil {
				return err
			}
			d.Register(hook.New())
			ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
			defer cancel()
			return d.Run(ctx)
		},
	}
}

func daemonStartCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "start",
		Short: "데몬 백그라운드 시작",
		RunE: func(cmd *cobra.Command, args []string) error {
			if isRunning() {
				fmt.Println("daemon already running")
				return nil
			}
			exe, _ := os.Executable()
			child := exec.Command(exe, "daemon", "run")
			detachProcess(child)
			if err := child.Start(); err != nil {
				return fmt.Errorf("start daemon: %w", err)
			}
			addr := platform.SocketPath()
			for i := 0; i < 60; i++ {
				time.Sleep(50 * time.Millisecond)
				conn, err := ipc.Dial(addr)
				if err == nil {
					conn.Close()
					fmt.Printf("daemon started (pid %d)\n", child.Process.Pid)
					return nil
				}
			}
			return fmt.Errorf("daemon failed to start within 3 seconds")
		},
	}
}

func daemonStopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "데몬 종료",
		RunE: func(cmd *cobra.Command, args []string) error {
			pid, err := readPID()
			if err != nil {
				return fmt.Errorf("daemon not running")
			}
			proc, err := os.FindProcess(pid)
			if err != nil {
				return err
			}
			if runtime.GOOS == "windows" {
				proc.Kill()
			} else {
				proc.Signal(syscall.SIGTERM)
			}
			fmt.Println("daemon stopped")
			return nil
		},
	}
}

func daemonStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "데몬 상태 확인",
		Run: func(cmd *cobra.Command, args []string) {
			if isRunning() {
				pid, _ := readPID()
				fmt.Printf("daemon running (pid %d)\n", pid)
			} else {
				fmt.Println("daemon not running")
			}
		},
	}
}

func isRunning() bool {
	pid, err := readPID()
	if err != nil {
		return false
	}
	return platform.IsProcessAlive(pid)
}

func readPID() (int, error) {
	data, err := os.ReadFile(platform.PIDFilePath())
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}
