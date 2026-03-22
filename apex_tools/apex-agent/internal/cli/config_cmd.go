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
			fmt.Printf("idle_timeout: %s\n", cfg.Daemon.IdleTimeout)
			fmt.Printf("socket_path: %s\n", cfg.Daemon.SocketPath)
			fmt.Printf("db_path: %s\n", cfg.Store.DBPath)
			fmt.Printf("log.level: %s\n", cfg.Log.Level)
			fmt.Printf("log.file: %s\n", cfg.Log.File)
			fmt.Printf("log.audit: %v\n", cfg.Log.Audit)
			return nil
		},
	}
}
