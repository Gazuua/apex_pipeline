// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package version

// Version is set at build time via ldflags:
//
//	go build -ldflags "-X github.com/Gazuua/apex_pipeline/apex_tools/apex-agent/internal/version.Version=v1.0.0"
var Version = "dev"
