// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/workflow"
)

// getBranchID extracts the workspace branch identifier from the current directory.
func getBranchID() string {
	cwd, _ := os.Getwd()
	return platform.WorkspaceID(cwd)
}

func handoffCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "handoff",
		Short: "브랜치 핸드오프 관리",
	}

	notify := &cobra.Command{
		Use:   "notify",
		Short: "핸드오프 알림 전송",
	}
	startCmd := handoffNotifyStartCmd()
	startCmd.AddCommand(handoffNotifyStartJobCmd())
	notify.AddCommand(startCmd)
	notify.AddCommand(handoffNotifyDesignCmd())
	notify.AddCommand(handoffNotifyPlanCmd())
	notify.AddCommand(handoffNotifyMergeCmd())
	notify.AddCommand(handoffNotifyDropCmd())

	cmd.AddCommand(notify)
	cmd.AddCommand(handoffStatusCmd())
	cmd.AddCommand(handoffBacklogCheckCmd())
	return cmd
}

// ── handoff notify start ──

func handoffNotifyStartCmd() *cobra.Command {
	var (
		summary    string
		branchName string
		backlogs   []int
		scopes     string
		skipDesign bool
	)

	cmd := &cobra.Command{
		Use:   "start",
		Short: "작업 착수 알림 (백로그 연결)",
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(backlogs) == 0 {
				return fmt.Errorf("백로그 작업은 --backlog 필수. 비백로그 작업은 'notify start job' 사용")
			}
			return doNotifyStart(branchName, summary, backlogs, scopes, skipDesign)
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "작업 요약 (필수)")
	cmd.Flags().StringVar(&branchName, "branch-name", "", "git 브랜치명 (필수, feature/* 또는 bugfix/*)")
	cmd.Flags().IntSliceVar(&backlogs, "backlog", nil, "백로그 번호 (복수 가능)")
	cmd.Flags().StringVar(&scopes, "scopes", "", "영향 스코프 (예: core,shared)")
	cmd.Flags().BoolVar(&skipDesign, "skip-design", false, "설계 단계 스킵 (바로 implementing)")
	_ = cmd.MarkFlagRequired("summary")
	_ = cmd.MarkFlagRequired("branch-name")
	_ = cmd.MarkFlagRequired("scopes")

	return cmd
}

// ── handoff notify start job ──

func handoffNotifyStartJobCmd() *cobra.Command {
	var (
		summary    string
		branchName string
		scopes     string
		skipDesign bool
	)

	cmd := &cobra.Command{
		Use:   "job",
		Short: "비백로그 작업 착수 알림",
		RunE: func(cmd *cobra.Command, args []string) error {
			return doNotifyStart(branchName, summary, nil, scopes, skipDesign)
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "작업 요약 (필수)")
	cmd.Flags().StringVar(&branchName, "branch-name", "", "git 브랜치명 (필수, feature/* 또는 bugfix/*)")
	cmd.Flags().StringVar(&scopes, "scopes", "", "영향 스코프 (예: core,shared)")
	cmd.Flags().BoolVar(&skipDesign, "skip-design", false, "설계 단계 스킵 (바로 implementing)")
	_ = cmd.MarkFlagRequired("summary")
	_ = cmd.MarkFlagRequired("branch-name")
	_ = cmd.MarkFlagRequired("scopes")

	return cmd
}

// doNotifyStart handles the common logic for notify start and notify start job.
func doNotifyStart(branchName, summary string, backlogs []int, scopes string, skipDesign bool) error {
	branch := getBranchID()

	if backlogs == nil {
		backlogs = []int{}
	}
	params := map[string]any{
		"branch":      branch,
		"workspace":   branch,
		"branch_name": branchName,
		"summary":     summary,
		"backlog_ids": backlogs,
		"scopes":      scopes,
		"skip_design": skipDesign,
	}

	root, err := projectRoot()
	if err != nil {
		return fmt.Errorf("프로젝트 루트를 찾을 수 없습니다: %w", err)
	}

	// backlog manager (best-effort — import은 non-fatal)
	_, mgr, cleanup, mgrErr := openBacklogStore()
	if mgrErr != nil {
		mgr = nil
	} else {
		defer cleanup()
	}

	if err := workflow.StartPipeline(branchName, params, root, mgr, ipcWrapper); err != nil {
		return err
	}

	mode := "scopes=" + scopes
	if len(backlogs) == 0 {
		mode = "job mode"
	}
	fmt.Printf("[handoff] branch started (branch=%s, git=%s, %s)\n",
		branch, branchName, mode)
	return nil
}

// ipcWrapper adapts sendHandoffRequest to workflow.IPCFunc.
func ipcWrapper(action string, params map[string]any) (map[string]any, error) {
	return sendHandoffRequest(action, params)
}

// ── handoff notify design ──

func handoffNotifyDesignCmd() *cobra.Command {
	var summary string

	cmd := &cobra.Command{
		Use:   "design",
		Short: "설계 확정 알림",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"type":      "design",
				"summary":   summary,
			}
			if _, err := sendHandoffRequest("notify-transition", params); err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			fmt.Printf("[handoff] status → design-notified (branch=%s)\n", branch)
			return nil
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "설계 요약 (필수)")
	_ = cmd.MarkFlagRequired("summary")

	return cmd
}

// ── handoff notify plan ──

func handoffNotifyPlanCmd() *cobra.Command {
	var summary string

	cmd := &cobra.Command{
		Use:   "plan",
		Short: "구현 계획 확정 알림",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"type":      "plan",
				"summary":   summary,
			}
			if _, err := sendHandoffRequest("notify-transition", params); err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			fmt.Printf("[handoff] status → implementing (branch=%s)\n", branch)
			return nil
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "계획 요약 (필수)")
	_ = cmd.MarkFlagRequired("summary")

	return cmd
}

// ── handoff notify merge ──

func handoffNotifyMergeCmd() *cobra.Command {
	var summary string

	cmd := &cobra.Command{
		Use:   "merge",
		Short: "머지 완료 알림",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"summary":   summary,
			}

			root, err := projectRoot()
			if err != nil {
				root = "."
			}

			_, mgr, cleanup, mgrErr := openBacklogStore()
			if mgrErr != nil {
				mgr = nil
			} else {
				defer cleanup()
			}

			if err := workflow.MergePipeline(params, root, mgr, ipcWrapper); err != nil {
				return err
			}
			fmt.Printf("[handoff] branch merged (branch=%s)\n", branch)
			return nil
		},
	}

	cmd.Flags().StringVar(&summary, "summary", "", "머지 요약 (필수)")
	_ = cmd.MarkFlagRequired("summary")

	return cmd
}

// ── handoff notify drop ──

func handoffNotifyDropCmd() *cobra.Command {
	var reason string

	cmd := &cobra.Command{
		Use:   "drop",
		Short: "작업 중도 포기",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			params := map[string]any{
				"branch":    branch,
				"workspace": branch,
				"reason":    reason,
			}

			root, err := projectRoot()
			if err != nil {
				root = "."
			}

			if err := workflow.DropPipeline(params, root, ipcWrapper); err != nil {
				return err
			}
			fmt.Printf("[handoff] branch dropped (branch=%s, reason=%s)\n", branch, reason)
			return nil
		},
	}

	cmd.Flags().StringVar(&reason, "reason", "", "포기 사유 (필수)")
	_ = cmd.MarkFlagRequired("reason")

	return cmd
}

// ── handoff status ──

func handoffStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "현재 브랜치 핸드오프 상태 조회",
		RunE: func(cmd *cobra.Command, args []string) error {
			branch := getBranchID()
			result, err := sendHandoffRequest("get-branch", map[string]any{"branch": branch})
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}

			raw, _ := json.Marshal(result["branch"])
			if string(raw) == "null" || len(raw) == 0 {
				fmt.Printf("[handoff] branch=%s: not registered\n", branch)
				return nil
			}

			var b struct {
				Branch     string `json:"Branch"`
				Workspace  string `json:"Workspace"`
				Status     string `json:"Status"`
				BacklogIDs []int  `json:"BacklogIDs"`
				Summary    string `json:"Summary"`
				CreatedAt  string `json:"CreatedAt"`
				UpdatedAt  string `json:"UpdatedAt"`
			}
			if err := json.Unmarshal(raw, &b); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}

			fmt.Printf("[handoff] branch=%s status=%s", b.Branch, b.Status)
			if len(b.BacklogIDs) > 0 {
				fmt.Printf(" backlogs=")
				for i, id := range b.BacklogIDs {
					if i > 0 {
						fmt.Printf(",")
					}
					fmt.Printf("BACKLOG-%d", id)
				}
			}
			if b.Summary != "" {
				fmt.Printf(" summary=%q", b.Summary)
			}
			fmt.Println()
			if b.UpdatedAt != "" {
				fmt.Printf("          updated=%s\n", b.UpdatedAt)
			}
			return nil
		},
	}
}

// ── handoff backlog-check ──

func handoffBacklogCheckCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "backlog-check N",
		Short: "백로그 번호 사용 여부 확인",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			var id int
			if _, err := fmt.Sscanf(args[0], "%d", &id); err != nil {
				return fmt.Errorf("N must be an integer: %s", args[0])
			}
			result, err := sendHandoffRequest("backlog-check", map[string]any{"backlog_id": id})
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}

			available, _ := result["available"].(bool)
			usedBy, _ := result["branch"].(string)

			if available {
				fmt.Printf("BACKLOG-%d: AVAILABLE\n", id)
			} else {
				fmt.Printf("BACKLOG-%d: IN USE by %s\n", id, usedBy)
			}
			return nil
		},
	}
}
