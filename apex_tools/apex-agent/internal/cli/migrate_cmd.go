// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package cli

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/modules/backlog"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/platform"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/store"
)

func migrateCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "migrate",
		Short: "데이터 마이그레이션",
	}
	cmd.AddCommand(migrateBacklogCmd())
	return cmd
}

func migrateBacklogCmd() *cobra.Command {
	var backlogPath, historyPath string

	cmd := &cobra.Command{
		Use:   "backlog",
		Short: "BACKLOG.md + BACKLOG_HISTORY.md → DB 마이그레이션",
		RunE: func(cmd *cobra.Command, args []string) error {
			// Open store directly (no daemon needed for migration)
			if err := platform.EnsureDataDir(); err != nil {
				return err
			}
			s, err := store.Open(platform.DBPath())
			if err != nil {
				return fmt.Errorf("open store: %w", err)
			}
			defer s.Close()

			// Run migrations
			mig := store.NewMigrator(s)
			mod := backlog.New(s)
			mod.RegisterSchema(mig)
			if err := mig.Migrate(); err != nil {
				return fmt.Errorf("migrate schema: %w", err)
			}

			mgr := backlog.NewManager(s)
			total := 0

			// Import BACKLOG.md
			if data, err := os.ReadFile(backlogPath); err == nil {
				items, err := backlog.ParseBacklogMD(string(data))
				if err != nil {
					return fmt.Errorf("parse BACKLOG.md: %w", err)
				}
				count, err := mgr.ImportItems(items)
				if err != nil {
					return fmt.Errorf("import backlog: %w", err)
				}
				fmt.Printf("BACKLOG.md: %d items imported\n", count)
				total += count
			} else {
				fmt.Printf("BACKLOG.md not found: %s\n", backlogPath)
			}

			// Import BACKLOG_HISTORY.md
			if data, err := os.ReadFile(historyPath); err == nil {
				items, err := backlog.ParseBacklogHistoryMD(string(data))
				if err != nil {
					return fmt.Errorf("parse BACKLOG_HISTORY.md: %w", err)
				}
				count, err := mgr.ImportItems(items)
				if err != nil {
					return fmt.Errorf("import history: %w", err)
				}
				fmt.Printf("BACKLOG_HISTORY.md: %d items imported\n", count)
				total += count
			} else {
				fmt.Printf("BACKLOG_HISTORY.md not found: %s\n", historyPath)
			}

			fmt.Printf("Total: %d items imported\n", total)
			return nil
		},
	}

	cmd.Flags().StringVar(&backlogPath, "backlog", "docs/BACKLOG.md", "BACKLOG.md 파일 경로")
	cmd.Flags().StringVar(&historyPath, "history", "docs/BACKLOG_HISTORY.md", "BACKLOG_HISTORY.md 파일 경로")
	return cmd
}
