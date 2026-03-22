// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package main

import "github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/cli"

var Version = "dev"

func main() {
	cli.Version = Version
	cli.Execute()
}
