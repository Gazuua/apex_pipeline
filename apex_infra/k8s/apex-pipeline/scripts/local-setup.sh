#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# Apex Pipeline — minikube 로컬 환경 원클릭 셋업
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SCRIPT_DIR"

echo "=== Apex Pipeline — Local K8s Setup ==="

# 1. minikube 상태 확인 + 시작
if ! minikube status > /dev/null 2>&1; then
    echo "[1/6] Starting minikube..."
    minikube start --memory=4096 --cpus=2
else
    echo "[1/6] minikube already running"
fi

# 2. Ingress addon
echo "[2/6] Enabling ingress addon..."
minikube addons enable ingress 2>/dev/null || true

# 3. Helm dependencies
echo "[3/6] Updating Helm dependencies..."
helm dependency update .

# 4. Infra release (apex-infra namespace)
echo "[4/6] Installing infra release (apex-infra)..."
helm upgrade --install apex-infra . \
    -n apex-infra --create-namespace \
    --set gateway.enabled=false \
    --set auth-svc.enabled=false \
    --set chat-svc.enabled=false \
    --set log-svc.enabled=false \
    --wait --timeout 5m

# 5. Services release (apex-services namespace)
echo "[5/6] Installing services release (apex-services)..."
helm upgrade --install apex-services . \
    -n apex-services --create-namespace \
    --set kafka.enabled=false \
    --set redis-auth.enabled=false \
    --set redis-ratelimit.enabled=false \
    --set redis-chat.enabled=false \
    --set redis-pubsub.enabled=false \
    --set postgresql.enabled=false \
    --set pgbouncer.enabled=false \
    --wait --timeout 5m

# 6. Helm test
echo "[6/6] Running helm tests..."
helm test apex-services -n apex-services || true

echo ""
echo "=== Setup complete ==="
echo "Run 'minikube tunnel' in a separate terminal for Ingress access."
echo "Then connect to: wss://apex.local"
