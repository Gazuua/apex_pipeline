// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cli"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/daemon"
	"github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/ipc"
)

var Version = "dev"

func main() {
	// 임시 디버그: hook 호출 시 진입 로그
	if len(os.Args) > 1 && os.Args[1] == "hook" {
		f, _ := os.OpenFile(hookDebugLog(), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if f != nil {
			fmt.Fprintf(f, "%s args=%v pid=%d\n", time.Now().Format("15:04:05.000"), os.Args, os.Getpid())
			f.Close()
		}
	}

	cli.Version = Version
	daemon.Version = Version
	ipc.Version = Version
	cli.Execute()
}

func hookDebugLog() string {
	if d := os.Getenv("LOCALAPPDATA"); d != "" {
		return d + "\\apex-agent\\hook-debug.log"
	}
	return strings.TrimRight(os.TempDir(), string(os.PathSeparator)) + "/apex-agent-hook-debug.log"
}
