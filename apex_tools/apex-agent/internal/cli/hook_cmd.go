// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"io"
	"os"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/hook"
)

func hookCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "hook",
		Short: "Hook 검증 게이트",
	}
	cmd.AddCommand(hookValidateBuildCmd())
	cmd.AddCommand(hookValidateMergeCmd())
	cmd.AddCommand(hookValidateHandoffCmd())
	cmd.AddCommand(hookHandoffProbeCmd())
	return cmd
}

func hookValidateBuildCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-build",
		Short: "빌드 도구 직접 호출 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, _, err := readHookInput()
			if err != nil {
				return nil // parse error → allow (don't block on malformed input)
			}
			if err := hook.ValidateBuild(command); err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			return nil
		},
	}
}

func hookValidateMergeCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "validate-merge",
		Short: "머지 잠금 미획득 차단",
		RunE: func(cmd *cobra.Command, args []string) error {
			command, cwd, err := readHookInput()
			if err != nil {
				return nil
			}
			if err := hook.ValidateMerge(command, cwd); err != nil {
				fmt.Fprintln(os.Stderr, err.Error())
				os.Exit(2)
			}
			return nil
		},
	}
}

// readHookInput reads Claude Code hook JSON from stdin.
// Returns (command, cwd, error).
func readHookInput() (string, string, error) {
	data, err := io.ReadAll(os.Stdin)
	if err != nil {
		return "", "", err
	}

	var input struct {
		ToolInput struct {
			Command string `json:"command"`
		} `json:"tool_input"`
		Cwd string `json:"cwd"`
	}
	if err := json.Unmarshal(data, &input); err != nil {
		return "", "", err
	}

	return input.ToolInput.Command, input.Cwd, nil
}
