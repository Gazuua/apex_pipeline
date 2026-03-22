// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

package platform

import (
	"path/filepath"
	"runtime"
	"strings"
)

// NormalizePath normalizes a file path for the current platform.
// On Windows, handles MSYS-style paths (/c/Users/...) and forward slashes.
func NormalizePath(p string) string {
	if runtime.GOOS == "windows" && len(p) >= 3 && p[0] == '/' && p[2] == '/' {
		drive := strings.ToUpper(string(p[1]))
		p = drive + ":" + p[2:]
	}
	return filepath.Clean(filepath.FromSlash(p))
}
