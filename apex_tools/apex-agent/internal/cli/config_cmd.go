// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/config"
)

func configCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "config",
		Short: "설정 관리",
	}
	cmd.AddCommand(configInitCmd())
	cmd.AddCommand(configShowCmd())
	return cmd
}

func configInitCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "init",
		Short: "기본 config.toml 생성",
		RunE: func(cmd *cobra.Command, args []string) error {
			path := config.DefaultPath()
			if err := config.WriteDefault(path); err != nil {
				return err
			}
			fmt.Printf("config written to %s\n", path)
			return nil
		},
	}
}

func configShowCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "show",
		Short: "현재 설정 출력",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, err := config.Load(config.DefaultPath())
			if err != nil {
				return err
			}
			fmt.Printf("[daemon]\n")
			fmt.Printf("  socket_path: %s\n", cfg.Daemon.SocketPath)
			fmt.Printf("[store]\n")
			fmt.Printf("  db_path: %s\n", cfg.Store.DBPath)
			fmt.Printf("[queue]\n")
			fmt.Printf("  stale_timeout: %v\n", cfg.Queue.StaleTimeout)
			fmt.Printf("  poll_interval: %v\n", cfg.Queue.PollInterval)
			fmt.Printf("[log]\n")
			fmt.Printf("  level: %s\n", cfg.Log.Level)
			fmt.Printf("  max_days: %d\n", cfg.Log.MaxDays)
			fmt.Printf("  audit: %v\n", cfg.Log.Audit)
			fmt.Printf("[build]\n")
			fmt.Printf("  command: %s\n", cfg.Build.Command)
			fmt.Printf("  presets: %v\n", cfg.Build.Presets)
			fmt.Printf("[http]\n")
			fmt.Printf("  enabled: %v\n", cfg.HTTP.Enabled)
			fmt.Printf("  addr: %s\n", cfg.HTTP.Addr)
			return nil
		},
	}
}
