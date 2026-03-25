// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/httpd"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	backlogmod "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	handoffmod "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/handoff"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	queuemod "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/queue"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
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
			// Clear maintenance lock — daemon is being explicitly started.
			os.Remove(platform.MaintenanceFilePath())

			appCfg, err := config.Load(config.DefaultPath())
			if err != nil {
				return err
			}

			// Set up logging — daily rotation (logs/YYYYMMDD.log).
			logDir := filepath.Join(platform.DataDir(), "logs")
			dailyWriter, dwErr := log.NewDailyWriter(log.DailyWriterConfig{
				Dir:     logDir,
				MaxDays: appCfg.Log.MaxDays,
			})
			if dwErr != nil {
				return fmt.Errorf("daily log writer: %w", dwErr)
			}
			defer dailyWriter.Close()
			logWriter := io.MultiWriter(os.Stderr, dailyWriter)
			log.Init(log.LogConfig{Level: appCfg.Log.Level, Writer: logWriter})

			daemonCfg := daemon.Config{
				DBPath:      appCfg.Store.DBPath,
				PIDFilePath: platform.PIDFilePath(),
				SocketAddr:  appCfg.Daemon.SocketPath,
				HTTP:        appCfg.HTTP,
			}
			// Fill empty values with platform defaults
			if daemonCfg.DBPath == "" {
				daemonCfg.DBPath = platform.DBPath()
			}
			if daemonCfg.SocketAddr == "" {
				daemonCfg.SocketAddr = platform.SocketPath()
			}
			d, err := daemon.New(daemonCfg)
			if err != nil {
				return err
			}
			backlogMod := backlogmod.New(d.Store())
			queueMod := queuemod.New(d.Store())
			handoffMod := handoffmod.New(d.Store(), backlogMod.Manager(), queueMod.Manager())

			d.Register(hook.New())
			d.Register(backlogMod)
			d.Register(handoffMod)
			d.Register(queueMod)

			// Junction 콜백 주입: backlog 모듈이 handoff 소유 branch_backlogs를 직접 접근하지 않도록 콜백 위임
			backlogMod.Manager().SetJunctionCleaner(func(ctx context.Context, q store.Querier, backlogID int) error {
				_, err := q.Exec(ctx, `DELETE FROM branch_backlogs WHERE backlog_id = ?`, backlogID)
				return err
			})
			backlogMod.Manager().SetJunctionCreator(func(ctx context.Context, q store.Querier, branch string, backlogID int) error {
				_, err := q.Exec(ctx, `INSERT OR IGNORE INTO branch_backlogs (branch, backlog_id) VALUES (?, ?)`, branch, backlogID)
				return err
			})

			// HTTP 서버 팩토리 설정: 어댑터를 통해 모듈 Manager를 httpd에 주입
			bqa := &backlogQuerierAdapter{mgr: backlogMod.Manager()}
			hqa := &handoffQuerierAdapter{mgr: handoffMod.Manager()}
			qqa := &queueQuerierAdapter{mgr: queueMod.Manager()}
			d.SetHTTPServerFactory(func(addr string) *httpd.Server {
				return httpd.New(bqa, hqa, qqa, d.Router(), addr)
			})
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
			// Clear maintenance lock — daemon is being explicitly started.
			os.Remove(platform.MaintenanceFilePath())

			if isRunning() {
				fmt.Println("daemon already running")
				return nil
			}
			pid, err := startDaemonProcess()
			if err != nil {
				return err
			}
			fmt.Printf("daemon started (pid %d)\n", pid)
			return nil
		},
	}
}

func daemonStopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "데몬 종료",
		RunE: func(cmd *cobra.Command, args []string) error {
			// Write maintenance lock — suppress auto-restart during binary upgrade.
			_ = os.WriteFile(platform.MaintenanceFilePath(), []byte("maintenance"), 0o600)

			// IPC로 graceful shutdown 먼저 시도
			client := ipc.NewClient(platform.SocketPath())
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			if _, err := client.Send(ctx, "daemon", "shutdown", nil, ""); err == nil {
				// shutdown 요청 성공 — 데몬에 종료 시간 부여
				time.Sleep(500 * time.Millisecond)
				fmt.Println("daemon stopped (graceful)")
				return nil
			}

			// IPC 실패 시 PID 기반 Kill fallback
			pid, err := readPID()
			if err != nil {
				os.Remove(platform.MaintenanceFilePath()) // stop 완전 실패 → lock 정리
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
