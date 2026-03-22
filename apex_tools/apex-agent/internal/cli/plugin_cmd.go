// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

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

func pluginSetupCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "setup",
		Short: "auto-review 플러그인 등록",
		Long:  "SessionStart 훅에서 호출 — apex-auto-review 플러그인을 ~/.claude에 idempotent하게 등록합니다.",
		RunE: func(cmd *cobra.Command, args []string) error {
			cwd, err := os.Getwd()
			if err != nil {
				return fmt.Errorf("getwd: %w", err)
			}
			return plugin.Setup(cwd)
		},
	}
}
