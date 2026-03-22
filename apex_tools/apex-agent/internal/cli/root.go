// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"
)

var Version = "dev"

func Execute() {
	root := &cobra.Command{
		Use:   "apex-agent",
		Short: "apex-agent: 개발 자동화 플랫폼",
		CompletionOptions: cobra.CompletionOptions{
			DisableDefaultCmd: true,
		},
	}

	root.AddCommand(daemonCmd())
	root.AddCommand(versionCmd())
	root.AddCommand(hookCmd())
	root.AddCommand(backlogCmd())

	if err := root.Execute(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func versionCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "version",
		Short: "버전 출력",
		Run: func(cmd *cobra.Command, args []string) {
			fmt.Printf("apex-agent %s\n", Version)
		},
	}
}
