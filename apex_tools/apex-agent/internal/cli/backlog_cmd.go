// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
)

// parseID parses a CLI argument as a positive integer ID.
// Centralizes the repeated fmt.Sscanf pattern across backlog/handoff commands.
func parseID(arg string) (int, error) {
	var id int
	if _, err := fmt.Sscanf(arg, "%d", &id); err != nil {
		return 0, fmt.Errorf("ID must be an integer: %s", arg)
	}
	return id, nil
}

func backlogCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "backlog",
		Short: "백로그 관리",
	}
	cmd.AddCommand(backlogAddCmd())
	cmd.AddCommand(backlogShowCmd())
	cmd.AddCommand(backlogListCmd())
	cmd.AddCommand(backlogResolveCmd())
	cmd.AddCommand(backlogExportCmd())
	cmd.AddCommand(backlogCheckCmd())
	cmd.AddCommand(backlogReleaseCmd())
	cmd.AddCommand(backlogUpdateCmd())
	cmd.AddCommand(backlogFixCmd())
	return cmd
}

func sendBacklogRequest(action string, params any) (*ipc.Response, error) {
	return sendRequest("backlog", action, params, "")
}

// ── backlog add ──

func backlogAddCmd() *cobra.Command {
	var (
		title       string
		severity    string
		timeframe   string
		scope       string
		itemType    string
		description string
		related     string
		fix         bool
		noFix       bool
	)

	cmd := &cobra.Command{
		Use:   "add",
		Short: "백로그 항목 추가",
		RunE: func(cmd *cobra.Command, args []string) error {
			// 활성 브랜치에서 --fix/--no-fix 강제
			branch, _ := resolveCurrentBranch()
			if branch != "" {
				if !fix && !noFix {
					return fmt.Errorf("활성 브랜치 %q에서 backlog add 시 --fix 또는 --no-fix를 명시하세요.\n"+
						"  --fix    : 이 작업에서 즉시 수정 (FIXING 전이 + 브랜치 연결)\n"+
						"  --no-fix : 나중에 별도 작업으로 수정 (OPEN 유지)", branch)
				}
				if fix && noFix {
					return fmt.Errorf("--fix와 --no-fix는 동시에 사용할 수 없습니다")
				}
			}

			params := map[string]any{
				"title":       title,
				"severity":    severity,
				"timeframe":   timeframe,
				"scope":       scope,
				"type":        itemType,
				"description": description,
				"related":     related,
			}
			if fix && branch != "" {
				params["fix"] = true
				params["branch"] = branch
			}
			resp, err := sendBacklogRequest("add", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog add: %s", resp.Error)
			}
			var result struct {
				ID int `json:"id"`
			}
			if err := json.Unmarshal(resp.Data, &result); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}
			if fix {
				fmt.Printf("Added #%d (FIXING): %s\n", result.ID, title)
			} else {
				fmt.Printf("Added #%d: %s\n", result.ID, title)
			}

			return nil
		},
	}

	cmd.Flags().StringVar(&title, "title", "", "항목 제목 (필수)")
	cmd.Flags().StringVar(&severity, "severity", "", "등급: CRITICAL, MAJOR, MINOR (필수)")
	cmd.Flags().StringVar(&timeframe, "timeframe", "", "시간축: NOW, IN_VIEW, DEFERRED (필수)")
	cmd.Flags().StringVar(&scope, "scope", "", "스코프 (필수)")
	cmd.Flags().StringVar(&itemType, "type", "", "타입 (필수)")
	cmd.Flags().StringVar(&description, "description", "", "설명 (필수)")
	cmd.Flags().StringVar(&related, "related", "", "연관 번호 (쉼표 구분, 예: 50,89)")
	cmd.Flags().BoolVar(&fix, "fix", false, "즉시 수정 (FIXING 전이 + 현재 브랜치 연결)")
	cmd.Flags().BoolVar(&noFix, "no-fix", false, "나중에 수정 (OPEN 유지)")

	_ = cmd.MarkFlagRequired("title")
	_ = cmd.MarkFlagRequired("severity")
	_ = cmd.MarkFlagRequired("timeframe")
	_ = cmd.MarkFlagRequired("scope")
	_ = cmd.MarkFlagRequired("type")
	_ = cmd.MarkFlagRequired("description")

	return cmd
}

// resolveCurrentBranch resolves the active handoff branch for the current workspace.
// Returns empty string if no active branch (daemon down or not registered).
func resolveCurrentBranch() (string, error) {
	gitBranch, _ := platform.GitCurrentBranch("")
	branch, err := resolveHandoffBranch("", gitBranch)
	if err != nil {
		return "", nil // daemon unavailable — non-fatal, skip enforcement
	}
	return branch, nil
}

// ── backlog show ──

func backlogShowCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "show <ID>",
		Short: "백로그 항목 상세 조회",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			id, err := parseID(args[0])
			if err != nil {
				return err
			}
			resp, err := sendBacklogRequest("get", map[string]any{"id": id})
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog show: %s", resp.Error)
			}
			if resp.Data == nil || string(resp.Data) == "null" {
				return fmt.Errorf("backlog item %d not found", id)
			}
			var item struct {
				ID          int    `json:"id"`
				Title       string `json:"title"`
				Severity    string `json:"severity"`
				Timeframe   string `json:"timeframe"`
				Scope       string `json:"scope"`
				Type        string `json:"type"`
				Description string `json:"description"`
				Related     string `json:"related"`
				Status      string `json:"status"`
				Resolution  string `json:"resolution"`
				ResolvedAt  string `json:"resolved_at"`
				CreatedAt   string `json:"created_at"`
				UpdatedAt   string `json:"updated_at"`
			}
			if err := json.Unmarshal(resp.Data, &item); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}

			fmt.Printf("ID:          %d\n", item.ID)
			fmt.Printf("Title:       %s\n", item.Title)
			fmt.Printf("Severity:    %s\n", item.Severity)
			fmt.Printf("Timeframe:   %s\n", item.Timeframe)
			fmt.Printf("Scope:       %s\n", item.Scope)
			fmt.Printf("Type:        %s\n", item.Type)
			fmt.Printf("Status:      %s\n", item.Status)
			if item.Related != "" {
				// "50,89" → "BACKLOG-50, BACKLOG-89"
				parts := strings.Split(item.Related, ",")
				var refs []string
				for _, p := range parts {
					p = strings.TrimSpace(p)
					if p != "" {
						refs = append(refs, "BACKLOG-"+p)
					}
				}
				fmt.Printf("Related:     %s\n", strings.Join(refs, ", "))
			}
			if item.Resolution != "" {
				fmt.Printf("Resolution:  %s\n", item.Resolution)
			}
			if item.ResolvedAt != "" {
				fmt.Printf("Resolved:    %s\n", item.ResolvedAt)
			}
			fmt.Printf("Created:     %s\n", item.CreatedAt)
			fmt.Printf("Updated:     %s\n", item.UpdatedAt)
			fmt.Printf("Description:\n")
			// Indent description lines
			for _, line := range strings.Split(item.Description, "\n") {
				fmt.Printf("  %s\n", line)
			}
			return nil
		},
	}
}

// ── backlog list ──

func backlogListCmd() *cobra.Command {
	var (
		timeframe string
		severity  string
		status    string
		verbose   bool
	)

	cmd := &cobra.Command{
		Use:   "list",
		Short: "백로그 목록 조회",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]any{
				"timeframe": timeframe,
				"severity":  severity,
				"status":    status,
			}
			resp, err := sendBacklogRequest("list", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog list: %s", resp.Error)
			}
			var items []struct {
				ID          int    `json:"id"`
				Title       string `json:"title"`
				Severity    string `json:"severity"`
				Timeframe   string `json:"timeframe"`
				Scope       string `json:"scope"`
				Type        string `json:"type"`
				Description string `json:"description"`
				Status      string `json:"status"`
			}
			if err := json.Unmarshal(resp.Data, &items); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}
			if len(items) == 0 {
				fmt.Println("(백로그 항목 없음)")
				return nil
			}
			fmt.Printf("%-6s %-10s %-10s %-10s %-10s %-10s %s\n",
				"ID", "STATUS", "SEVERITY", "TIMEFRAME", "SCOPE", "TYPE", "TITLE")
			fmt.Println(strings.Repeat("-", 80))
			for _, item := range items {
				fmt.Printf("%-6d %-10s %-10s %-10s %-10s %-10s %s\n",
					item.ID, item.Status, item.Severity, item.Timeframe, item.Scope, item.Type, item.Title)
				if verbose && item.Description != "" {
					// Show first line of description, indented
					firstLine := item.Description
					if idx := strings.Index(firstLine, "\n"); idx > 0 {
						firstLine = firstLine[:idx]
					}
					if len(firstLine) > 80 {
						firstLine = firstLine[:77] + "..."
					}
					fmt.Printf("       %s\n", firstLine)
				}
			}
			return nil
		},
	}

	cmd.Flags().StringVar(&timeframe, "timeframe", "", "필터: NOW, IN_VIEW, DEFERRED")
	cmd.Flags().StringVar(&severity, "severity", "", "필터: CRITICAL, MAJOR, MINOR")
	cmd.Flags().StringVar(&status, "status", "OPEN", "필터: OPEN, FIXING, RESOLVED")
	cmd.Flags().BoolVarP(&verbose, "verbose", "v", false, "description 첫 줄 표시")

	return cmd
}

// ── backlog resolve ──

func backlogResolveCmd() *cobra.Command {
	var resolution string

	cmd := &cobra.Command{
		Use:   "resolve ID",
		Short: "백로그 항목 해결 처리",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			id, err := parseID(args[0])
			if err != nil {
				return err
			}
			params := map[string]any{
				"id":         id,
				"resolution": resolution,
			}
			resp, err := sendBacklogRequest("resolve", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog resolve: %s", resp.Error)
			}
			fmt.Printf("Resolved #%d: %s\n", id, resolution)

			return nil
		},
	}

	cmd.Flags().StringVar(&resolution, "resolution", "", "해결 방식: FIXED, DOCUMENTED, WONTFIX, DUPLICATE, SUPERSEDED (필수)")
	_ = cmd.MarkFlagRequired("resolution")

	return cmd
}

// ── backlog export ──

func backlogExportCmd() *cobra.Command {
	var stdout bool

	cmd := &cobra.Command{
		Use:   "export",
		Short: "DB → docs/BACKLOG.json 동기화",
		Long: `DB 내용을 docs/BACKLOG.json에 직접 씁니다.
--stdout 플래그로 JSON을 stdout에 출력합니다 (디버깅용).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			// IPC로 데몬에서 export JSON을 가져옴
			resp, err := sendBacklogRequest("export", nil)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog export: %s", resp.Error)
			}
			var result struct {
				Content string `json:"content"`
			}
			if err := json.Unmarshal(resp.Data, &result); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}

			if stdout {
				fmt.Fprint(os.Stdout, result.Content)
				return nil
			}

			// 기본: 파일 직접 쓰기
			root, err := projectRoot()
			if err != nil {
				return fmt.Errorf("프로젝트 루트를 찾을 수 없습니다: %w", err)
			}

			jsonPath := filepath.Join(root, "docs", "BACKLOG.json")
			if mkErr := os.MkdirAll(filepath.Join(root, "docs"), 0o755); mkErr != nil {
				return mkErr
			}
			if writeErr := os.WriteFile(jsonPath, []byte(result.Content), 0o644); writeErr != nil {
				return writeErr
			}

			// 레거시 MD 마이그레이션: 존재하면 삭제
			for _, f := range []string{"BACKLOG.md", "BACKLOG_HISTORY.md"} {
				mdPath := filepath.Join(root, "docs", f)
				if _, statErr := os.Stat(mdPath); statErr == nil {
					os.Remove(mdPath)
				}
			}

			fmt.Println("[export] docs/BACKLOG.json 갱신 완료")
			return nil
		},
	}

	cmd.Flags().BoolVar(&stdout, "stdout", false, "stdout 출력 (디버깅용, 파일 쓰기 안 함)")
	return cmd
}

// ── backlog update ──

func backlogUpdateCmd() *cobra.Command {
	var title, severity, timeframe, scope, itemType, description, related string
	var position int

	cmd := &cobra.Command{
		Use:   "update ID",
		Short: "백로그 항목 메타데이터 수정",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			id, err := parseID(args[0])
			if err != nil {
				return err
			}

			fields := make(map[string]string)
			if cmd.Flags().Changed("title") {
				fields["title"] = title
			}
			if cmd.Flags().Changed("severity") {
				fields["severity"] = strings.ToUpper(severity)
			}
			if cmd.Flags().Changed("timeframe") {
				fields["timeframe"] = strings.ToUpper(strings.ReplaceAll(timeframe, " ", "_"))
			}
			if cmd.Flags().Changed("scope") {
				fields["scope"] = scope
			}
			if cmd.Flags().Changed("type") {
				fields["type"] = strings.ToUpper(strings.ReplaceAll(itemType, "-", "_"))
			}
			if cmd.Flags().Changed("description") {
				fields["description"] = description
			}
			if cmd.Flags().Changed("related") {
				fields["related"] = related
			}
			if cmd.Flags().Changed("position") {
				fields["position"] = fmt.Sprintf("%d", position)
			}

			if len(fields) == 0 {
				return fmt.Errorf("최소 1개 필드를 지정하세요 (--title, --severity, --description 등)")
			}

			params := map[string]any{
				"id":     id,
				"fields": fields,
			}
			resp, err := sendBacklogRequest("update", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog update: %s", resp.Error)
			}
			fmt.Printf("Updated #%d: %d fields\n", id, len(fields))

			return nil
		},
	}

	cmd.Flags().StringVar(&title, "title", "", "제목")
	cmd.Flags().StringVar(&severity, "severity", "", "등급 (CRITICAL/MAJOR/MINOR)")
	cmd.Flags().StringVar(&timeframe, "timeframe", "", "시간축 (NOW/IN_VIEW/DEFERRED)")
	cmd.Flags().StringVar(&scope, "scope", "", "스코프")
	cmd.Flags().StringVar(&itemType, "type", "", "타입 (BUG/DESIGN_DEBT/...)")
	cmd.Flags().StringVar(&description, "description", "", "설명")
	cmd.Flags().StringVar(&related, "related", "", "연관 (예: 150,151)")
	cmd.Flags().IntVar(&position, "position", 0, "섹션 내 순서 (같은 timeframe 내 다른 항목 자동 재배치)")

	return cmd
}

// ── backlog release ──

func backlogReleaseCmd() *cobra.Command {
	var reason string

	cmd := &cobra.Command{
		Use:   "release ID",
		Short: "백로그 항목 착수 해제 (FIXING → OPEN)",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			id, err := parseID(args[0])
			if err != nil {
				return err
			}
			branch := getBranchID()
			params := map[string]any{"id": id, "reason": reason, "branch": branch}
			resp, err := sendBacklogRequest("release", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog release: %s", resp.Error)
			}
			fmt.Printf("Released #%d: %s\n", id, reason)

			return nil
		},
	}

	cmd.Flags().StringVar(&reason, "reason", "", "해제 사유 (필수)")
	_ = cmd.MarkFlagRequired("reason")

	return cmd
}

// ── backlog fix ──

func backlogFixCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "fix ID [ID...]",
		Short: "백로그 항목 착수 (OPEN → FIXING + 현재 브랜치 연결)",
		Args:  cobra.MinimumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			branch, err := resolveCurrentBranch()
			if err != nil || branch == "" {
				return fmt.Errorf("활성 브랜치가 없습니다. 'handoff notify start'로 먼저 브랜치를 등록하세요")
			}

			for _, arg := range args {
				id, err := parseID(arg)
				if err != nil {
					return err
				}
				params := map[string]any{"id": id, "branch": branch}
				resp, err := sendBacklogRequest("fix", params)
				if err != nil {
					return fmt.Errorf("daemon unavailable: %w", err)
				}
				if resp.Error != "" {
					return fmt.Errorf("backlog fix #%d: %s", id, resp.Error)
				}
				fmt.Printf("Fixed #%d → FIXING (branch: %s)\n", id, branch)
			}

			return nil
		},
	}
}

// ── backlog check ──

func backlogCheckCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "check ID",
		Short: "백로그 번호 사용 여부 확인",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			id, err := parseID(args[0])
			if err != nil {
				return err
			}
			params := map[string]any{"id": id}
			resp, err := sendBacklogRequest("check", params)
			if err != nil {
				return fmt.Errorf("daemon unavailable: %w", err)
			}
			if resp.Error != "" {
				return fmt.Errorf("backlog check: %s", resp.Error)
			}
			var result struct {
				Exists bool   `json:"exists"`
				Status string `json:"status"`
			}
			if err := json.Unmarshal(resp.Data, &result); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}
			if !result.Exists {
				fmt.Printf("BACKLOG-%d: AVAILABLE\n", id)
			} else {
				fmt.Printf("BACKLOG-%d: %s\n", id, result.Status)
			}
			return nil
		},
	}
}
