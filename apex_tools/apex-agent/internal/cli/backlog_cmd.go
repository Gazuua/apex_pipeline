// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func backlogCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "backlog",
		Short: "백로그 관리",
	}
	cmd.AddCommand(backlogAddCmd())
	cmd.AddCommand(backlogListCmd())
	cmd.AddCommand(backlogResolveCmd())
	cmd.AddCommand(backlogExportCmd())
	cmd.AddCommand(backlogCheckCmd())
	cmd.AddCommand(backlogReleaseCmd())
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
	)

	cmd := &cobra.Command{
		Use:   "add",
		Short: "백로그 항목 추가",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]any{
				"title":       title,
				"severity":    severity,
				"timeframe":   timeframe,
				"scope":       scope,
				"type":        itemType,
				"description": description,
				"related":     related,
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
			fmt.Printf("Added #%d: %s\n", result.ID, title)
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

	_ = cmd.MarkFlagRequired("title")
	_ = cmd.MarkFlagRequired("severity")
	_ = cmd.MarkFlagRequired("timeframe")
	_ = cmd.MarkFlagRequired("scope")
	_ = cmd.MarkFlagRequired("type")
	_ = cmd.MarkFlagRequired("description")

	return cmd
}

// ── backlog list ──

func backlogListCmd() *cobra.Command {
	var (
		timeframe string
		severity  string
		status    string
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
				ID          int    `json:"ID"`
				Title       string `json:"Title"`
				Severity    string `json:"Severity"`
				Timeframe   string `json:"Timeframe"`
				Scope       string `json:"Scope"`
				Type        string `json:"Type"`
				Description string `json:"Description"`
				Status      string `json:"Status"`
			}
			if err := json.Unmarshal(resp.Data, &items); err != nil {
				return fmt.Errorf("parse response: %w", err)
			}
			if len(items) == 0 {
				fmt.Println("(백로그 항목 없음)")
				return nil
			}
			fmt.Printf("%-6s %-10s %-10s %-10s %-10s %s\n",
				"ID", "SEVERITY", "TIMEFRAME", "SCOPE", "TYPE", "TITLE")
			fmt.Println(strings.Repeat("-", 70))
			for _, item := range items {
				fmt.Printf("%-6d %-10s %-10s %-10s %-10s %s\n",
					item.ID, item.Severity, item.Timeframe, item.Scope, item.Type, item.Title)
			}
			return nil
		},
	}

	cmd.Flags().StringVar(&timeframe, "timeframe", "", "필터: NOW, IN_VIEW, DEFERRED")
	cmd.Flags().StringVar(&severity, "severity", "", "필터: CRITICAL, MAJOR, MINOR")
	cmd.Flags().StringVar(&status, "status", "OPEN", "필터: OPEN, FIXING, RESOLVED")

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
			var id int
			if _, err := fmt.Sscanf(args[0], "%d", &id); err != nil {
				return fmt.Errorf("ID must be an integer: %s", args[0])
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

// openBacklogStore opens the DB directly (no daemon) and returns Store + Manager.
// The cleanup function must be called to close the store.
func openBacklogStore() (*store.Store, *backlog.Manager, func(), error) {
	if err := platform.EnsureDataDir(); err != nil {
		return nil, nil, nil, err
	}
	s, err := store.Open(platform.DBPath())
	if err != nil {
		return nil, nil, nil, fmt.Errorf("open store: %w", err)
	}
	mig := store.NewMigrator(s)
	mod := backlog.New(s)
	mod.RegisterSchema(mig)
	if err := mig.Migrate(); err != nil {
		s.Close()
		return nil, nil, nil, fmt.Errorf("migrate: %w", err)
	}
	return s, mod.Manager(), func() { s.Close() }, nil
}

// ── backlog export ──

func backlogExportCmd() *cobra.Command {
	var backlogPath, historyPath string
	var unsafe bool

	cmd := &cobra.Command{
		Use:   "export",
		Short: "BACKLOG.md 형식으로 출력 (import-first 안전 모드)",
		Long: `DB 내용을 BACKLOG.md 형식으로 출력합니다.

기본 동작 (안전 모드):
  1. 현재 BACKLOG.md + BACKLOG_HISTORY.md를 DB에 import (메타데이터만, 상태 불변)
  2. DB에서 export

이렇게 하면 DB가 유실되어도 MD 파일에서 복원 후 export하므로 데이터 손실이 없습니다.
--unsafe 플래그로 import 단계를 건너뛸 수 있습니다.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			// 직접 DB 접근 (daemon 불필요)
			_, mgr, cleanup, err := openBacklogStore()
			if err != nil {
				return err
			}
			defer cleanup()

			var content string

			if unsafe {
				// unsafe 모드: import 없이 바로 export
				var exportErr error
				content, exportErr = mgr.Export()
				if exportErr != nil {
					return exportErr
				}
			} else {
				// 안전 모드: SafeExport (import-first + 단일 트랜잭션)
				backlogData, _ := os.ReadFile(backlogPath)
				historyData, _ := os.ReadFile(historyPath)

				var imported int
				var exportErr error
				content, imported, exportErr = mgr.SafeExport(string(backlogData), string(historyData))
				if exportErr != nil {
					return exportErr
				}
				if imported > 0 {
					fmt.Fprintf(os.Stderr, "[export] import-first: %d items synced\n", imported)
				}
			}

			fmt.Fprint(os.Stdout, content)
			return nil
		},
	}

	cmd.Flags().StringVar(&backlogPath, "backlog", "docs/BACKLOG.md", "BACKLOG.md 파일 경로")
	cmd.Flags().StringVar(&historyPath, "history", "docs/BACKLOG_HISTORY.md", "BACKLOG_HISTORY.md 파일 경로")
	cmd.Flags().BoolVar(&unsafe, "unsafe", false, "import 단계 건너뛰기 (DB 유실 시 데이터 손실 위험)")
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
			var id int
			if _, err := fmt.Sscanf(args[0], "%d", &id); err != nil {
				return fmt.Errorf("ID must be an integer: %s", args[0])
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

// ── backlog check ──

func backlogCheckCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "check ID",
		Short: "백로그 번호 사용 여부 확인",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			var id int
			if _, err := fmt.Sscanf(args[0], "%d", &id); err != nil {
				return fmt.Errorf("ID must be an integer: %s", args[0])
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
