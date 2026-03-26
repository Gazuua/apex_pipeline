// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/log"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/session"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func sessionCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "session",
		Short: "세션 서버 관리",
	}
	cmd.AddCommand(sessionRunCmd())
	cmd.AddCommand(sessionStartCmd())
	cmd.AddCommand(sessionStopCmd())
	cmd.AddCommand(sessionStatusCmd())
	cmd.AddCommand(sessionSendCmd())
	return cmd
}

func sessionRunCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "run",
		Short: "세션 서버 포그라운드 실행 (디버깅용)",
		RunE: func(cmd *cobra.Command, args []string) error {
			if err := platform.EnsureDataDir(); err != nil {
				return err
			}

			appCfg, err := config.Load(config.DefaultPath())
			if err != nil {
				return err
			}

			if !appCfg.Session.Enabled {
				return fmt.Errorf("session server disabled in config")
			}

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

			dbPath := appCfg.Store.DBPath
			if dbPath == "" {
				dbPath = platform.DBPath()
			}
			st, err := store.Open(dbPath)
			if err != nil {
				return fmt.Errorf("open store: %w", err)
			}
			defer st.Close()

			srv := session.NewServer(appCfg.Session, appCfg.Workspace, st)

			ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
			defer cancel()
			return srv.Run(ctx)
		},
	}
}

func sessionStartCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "start",
		Short: "세션 서버 백그라운드 시작",
		RunE: func(cmd *cobra.Command, args []string) error {
			if isSessionRunning() {
				fmt.Println("session server already running")
				return nil
			}
			pid, err := startSessionProcess()
			if err != nil {
				return err
			}
			fmt.Printf("session server started (pid %d)\n", pid)
			return nil
		},
	}
}

func sessionStopCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "stop",
		Short: "세션 서버 종료",
		RunE: func(cmd *cobra.Command, args []string) error {
			pid, err := readSessionPID()
			if err != nil {
				return fmt.Errorf("session server not running")
			}
			proc, err := os.FindProcess(pid)
			if err != nil {
				return err
			}
			// On Windows, SIGTERM is not supported — use Kill.
			if err := proc.Signal(syscall.SIGTERM); err != nil {
				proc.Kill()
			}
			for i := 0; i < 50; i++ {
				time.Sleep(100 * time.Millisecond)
				if !platform.IsProcessAlive(pid) {
					break
				}
			}
			fmt.Println("session server stopped")
			return nil
		},
	}
}

func sessionStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "세션 서버 상태",
		Run: func(cmd *cobra.Command, args []string) {
			if isSessionRunning() {
				pid, _ := readSessionPID()
				fmt.Printf("session server running (pid %d)\n", pid)
			} else {
				fmt.Println("session server not running")
			}
		},
	}
}

func sessionSendCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "send <workspace_id> <text>",
		Short: "세션에 텍스트 전송 (stdin 주입)",
		Args:  cobra.MinimumNArgs(2),
		RunE: func(cmd *cobra.Command, args []string) error {
			wsID := args[0]
			text := strings.Join(args[1:], " ")

			appCfg, _ := config.Load(config.DefaultPath())
			addr := appCfg.Session.Addr
			if addr == "" {
				addr = "localhost:7601"
			}

			url := fmt.Sprintf("http://%s/api/session/%s/send", addr, wsID)
			body := fmt.Sprintf(`{"text":%q}`, text)
			resp, err := sendHTTPPost(url, body)
			if err != nil {
				return fmt.Errorf("send failed: %w", err)
			}
			fmt.Println(resp)
			return nil
		},
	}
}

func isSessionRunning() bool {
	pid, err := readSessionPID()
	if err != nil {
		return false
	}
	return platform.IsProcessAlive(pid)
}

func readSessionPID() (int, error) {
	data, err := os.ReadFile(platform.SessionPIDFilePath())
	if err != nil {
		return 0, err
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}

func startSessionProcess() (int, error) {
	exe, err := os.Executable()
	if err != nil {
		return 0, fmt.Errorf("resolve executable: %w", err)
	}
	child := exec.Command(exe, "session", "run")
	detachProcess(child)
	if err := child.Start(); err != nil {
		return 0, fmt.Errorf("start session server: %w", err)
	}

	appCfg, _ := config.Load(config.DefaultPath())
	addr := appCfg.Session.Addr
	if addr == "" {
		addr = "localhost:7601"
	}

	client := &http.Client{Timeout: 2 * time.Second}
	for i := 0; i < 100; i++ {
		time.Sleep(50 * time.Millisecond)
		resp, err := client.Get("http://" + addr + "/health")
		if err == nil {
			resp.Body.Close()
			return child.Process.Pid, nil
		}
	}
	return 0, fmt.Errorf("session server failed to start within 5 seconds")
}

func sendHTTPPost(url, jsonBody string) (string, error) {
	resp, err := http.Post(url, "application/json", strings.NewReader(jsonBody)) //nolint:gosec
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return string(body), nil
}
