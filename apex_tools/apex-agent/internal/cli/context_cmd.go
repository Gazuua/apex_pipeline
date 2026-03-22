// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/context"
)

func contextCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "context",
		Short: "세션 컨텍스트 출력",
		Long:  "SessionStart 훅에서 호출 — 현재 Git 상태, 브랜치 핸드오프 현황 등 프로젝트 컨텍스트를 stdout으로 출력합니다.",
		Run: func(cmd *cobra.Command, args []string) {
			cwd, err := os.Getwd()
			if err != nil {
				fmt.Fprintln(os.Stderr, "getwd:", err)
				os.Exit(1)
			}
			fmt.Print(context.Generate(cwd))
		},
	}
}
