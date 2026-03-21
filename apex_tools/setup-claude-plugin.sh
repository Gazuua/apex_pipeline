#!/bin/bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex-auto-review 플러그인 자동 설정 스크립트
# SessionStart 훅에서 호출 — idempotent (경로+버전 일치 시 즉시 종료, 불일치 시 갱신)
#
# 필요 조건: node.js (Claude Code 의존성)

set -euo pipefail

# 워크스페이스 루트 (이 스크립트의 부모 디렉토리)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 모든 경로 처리를 node 내부에서 수행 (MSYS 경로 변환 문제 방지)
node -e "
const fs = require('fs');
const path = require('path');
const os = require('os');

const PLUGIN_ID = 'apex-auto-review@apex-local';
const MARKETPLACE_ID = 'apex-local';

// 경로 설정 — os.homedir()으로 크로스플랫폼 처리
const claudeDir = path.join(os.homedir(), '.claude');
const pluginsDir = path.join(claudeDir, 'plugins');
const installedFile = path.join(pluginsDir, 'installed_plugins.json');
const knownFile = path.join(pluginsDir, 'known_marketplaces.json');
const settingsFile = path.join(claudeDir, 'settings.json');

// 워크스페이스 경로 (셸에서 전달)
const workspace = path.resolve('${WORKSPACE}'.replace(/\\\\$/,''));
const pluginPath = path.join(workspace, 'apex_tools', 'claude-plugin');
const marketplacePath = path.join(workspace, 'apex_tools');

// plugin.json에서 실제 버전 읽기
let pluginVersion = '1.0.0';
try {
  const pj = JSON.parse(fs.readFileSync(path.join(pluginPath, '.claude-plugin', 'plugin.json'), 'utf8'));
  if (pj.version) pluginVersion = pj.version;
} catch {}

function readJSON(filePath, fallback) {
  try {
    return JSON.parse(fs.readFileSync(filePath, 'utf8'));
  } catch {
    return fallback;
  }
}

function writeJSON(filePath, data) {
  fs.writeFileSync(filePath, JSON.stringify(data, null, 2) + '\n', 'utf8');
}

// --- 빠른 체크: 경로+버전 모두 일치하면 즉시 종료 ---
try {
  const installed = JSON.parse(fs.readFileSync(installedFile, 'utf8'));
  const entries = installed.plugins && installed.plugins[PLUGIN_ID];
  if (entries && entries.length > 0) {
    const entry = entries[0];
    if (path.resolve(entry.installPath) === path.resolve(pluginPath) && entry.version === pluginVersion) {
      process.exit(0);
    }
  }
} catch {}

// 디렉토리 확인
fs.mkdirSync(pluginsDir, { recursive: true });

// 1. known_marketplaces.json — 경로 항상 갱신
const known = readJSON(knownFile, {});
const needKnownUpdate = !known[MARKETPLACE_ID]
  || path.resolve(known[MARKETPLACE_ID].source?.path || '') !== path.resolve(marketplacePath);
if (needKnownUpdate) {
  known[MARKETPLACE_ID] = {
    source: { source: 'directory', path: marketplacePath },
    installLocation: marketplacePath,
    lastUpdated: new Date().toISOString()
  };
  writeJSON(knownFile, known);
}

// 2. installed_plugins.json — 경로+버전 갱신
const installed = readJSON(installedFile, { version: 2, plugins: {} });
const now = new Date().toISOString();
const existing = installed.plugins[PLUGIN_ID];
if (!existing || existing.length === 0) {
  installed.plugins[PLUGIN_ID] = [{
    scope: 'project',
    installPath: pluginPath,
    version: pluginVersion,
    installedAt: now,
    lastUpdated: now
  }];
  writeJSON(installedFile, installed);
} else {
  const entry = existing[0];
  const pathChanged = path.resolve(entry.installPath) !== path.resolve(pluginPath);
  const versionChanged = entry.version !== pluginVersion;
  if (pathChanged || versionChanged) {
    entry.installPath = pluginPath;
    entry.version = pluginVersion;
    entry.lastUpdated = now;
    writeJSON(installedFile, installed);
  }
}

// 3. settings.json (enabledPlugins)
const settings = readJSON(settingsFile, {});
if (!settings.enabledPlugins) settings.enabledPlugins = {};
if (!settings.enabledPlugins[PLUGIN_ID]) {
  settings.enabledPlugins[PLUGIN_ID] = true;
  writeJSON(settingsFile, settings);
}
"
