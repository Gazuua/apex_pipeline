#!/usr/bin/env bash
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# Apex Pipeline — 로컬 환경 정리
set -euo pipefail

echo "=== Apex Pipeline — Local K8s Teardown ==="

echo "[1/3] Uninstalling services release..."
helm uninstall apex-services -n apex-services 2>/dev/null || true

echo "[2/3] Uninstalling infra release..."
helm uninstall apex-infra -n apex-infra 2>/dev/null || true

echo "[3/3] Deleting namespaces..."
kubectl delete namespace apex-services apex-infra 2>/dev/null || true

echo ""
echo "Teardown complete. Run 'minikube stop' to stop the cluster."
