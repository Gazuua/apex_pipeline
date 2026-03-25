# v0.6.4 CI/CD 고도화 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CI/CD 파이프라인 완전 자동화 — 서비스 이미지 빌드/검증/배포, Argo Rollouts canary, 3단계 검증 파이프라인

**Architecture:** service.Dockerfile에 release preset 지원 추가, docker-bake.hcl에 조건부 태깅, docker-compose 스모크 테스트, Helm 차트에 Argo Rollouts CRD 지원, CI 워크플로우에 smoke-test/deploy 잡 추가

**Tech Stack:** GitHub Actions, Docker Buildx Bake, Helm 3, Argo Rollouts, minikube

**Spec:** `docs/apex_infra/plans/20260325_224153_v064_cicd_pipeline_design.md`
**Backlogs:** BACKLOG-178 (메인), BACKLOG-147, BACKLOG-190, BACKLOG-217

---

### Task 1: service.Dockerfile — CMAKE_PRESET 빌드 인자 추가

**Files:**
- Modify: `apex_infra/docker/service.Dockerfile`

- [ ] **Step 1: CMAKE_PRESET ARG 추가 + 빌드 경로 파라미터화**

```dockerfile
# Build stage에 추가
ARG CMAKE_PRESET=debug

# RUN 라인의 하드코딩된 "debug"를 ${CMAKE_PRESET}으로 교체
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --preset ${CMAKE_PRESET} -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/${CMAKE_PRESET} --target ${CMAKE_TARGET} \
    && mkdir -p /out \
    && cp build/Linux/${CMAKE_PRESET}/apex_services/${SERVICE_DIR}/${CMAKE_TARGET} /out/${CMAKE_TARGET}
```

기존 `cmake --preset debug`와 `build/Linux/debug` 경로를 `${CMAKE_PRESET}`으로 교체. 기본값 `debug`이므로 기존 동작 유지.

- [ ] **Step 2: Commit**

---

### Task 2: docker-bake.hcl — 조건부 태깅 + CMAKE_PRESET 변수

**Files:**
- Modify: `apex_infra/docker/docker-bake.hcl`

- [ ] **Step 1: IS_MAIN, CMAKE_PRESET 변수 추가**

```hcl
variable "IS_MAIN" {
  default = "false"
}

variable "CMAKE_PRESET" {
  default = "debug"
}
```

- [ ] **Step 2: service-base에 CMAKE_PRESET 전달**

```hcl
target "service-base" {
  dockerfile = "apex_infra/docker/service.Dockerfile"
  context    = "."
  args = {
    CMAKE_PRESET = CMAKE_PRESET
  }
  cache-from = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache"]
  cache-to   = ["type=registry,ref=${REGISTRY}/apex-pipeline-cache:buildcache,mode=max"]
}
```

- [ ] **Step 3: 각 서비스 target의 tags를 조건부로 변경**

삼항 연산자로 IS_MAIN일 때만 `:latest` 포함. 3개 서비스(gateway, auth-svc, chat-svc) 모두 동일 패턴.

```hcl
target "gateway" {
  inherits = ["service-base"]
  args = {
    CMAKE_TARGET = "apex_gateway"
    SERVICE_DIR  = "gateway"
    CONFIG_FILE  = "gateway_e2e.toml"
    CI_IMAGE_TAG = CI_IMAGE_TAG
  }
  tags = notequal(IS_MAIN, "true") ? [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
  ] : [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-gateway:latest",
  ]
}
```

- [ ] **Step 4: Commit**

---

### Task 3: Docker 전용 E2E config 파일 생성

**Files:**
- Create: `apex_services/tests/e2e/docker/gateway_docker.toml`
- Create: `apex_services/tests/e2e/docker/auth_svc_docker.toml`
- Create: `apex_services/tests/e2e/docker/chat_svc_docker.toml`

기존 E2E config를 복사한 후 3가지 변경:
1. RSA 키 경로: `apex_services/tests/keys/` → `keys/`
2. 인프라 엔드포인트: `localhost` → 컨테이너 이름, 포트 → 내부 포트
3. Gateway: `[metrics]` 섹션 추가 (health check용)

- [ ] **Step 1: gateway_docker.toml 생성**

`gateway_e2e.toml` 복사 후 변경:
- `public_key_file = "keys/test_rs256_pub.pem"`
- `brokers = "kafka:9092"`
- Redis 각각: `host = "redis-auth"/"redis-ratelimit"/"redis-pubsub"`, `port = 6379`
- `[metrics]` 섹션: `enabled = true`, `port = 8081`

- [ ] **Step 2: auth_svc_docker.toml 생성**

`auth_svc_e2e.toml` 복사 후 변경:
- `private_key_file = "keys/test_rs256.pem"`, `public_key_file = "keys/test_rs256_pub.pem"`
- `brokers = "kafka:9092"`
- `host = "redis-auth"`, `port = 6379`
- PostgreSQL: `host=postgres` (localhost → postgres)

- [ ] **Step 3: chat_svc_docker.toml 생성**

`chat_svc_e2e.toml` 복사 후 변경:
- `brokers = "kafka:9092"`
- `host = "redis-chat"/"redis-pubsub"`, `port = 6379`
- PostgreSQL: `host=postgres`

- [ ] **Step 4: Commit**

---

### Task 4: docker-compose.smoke.yml 생성

**Files:**
- Create: `apex_infra/docker/docker-compose.smoke.yml`

기존 `docker-compose.e2e.yml`을 기반으로 서비스 정의에서 `build:` → `image:` 교체, Docker config volume mount 추가.

- [ ] **Step 1: docker-compose.smoke.yml 작성**

인프라 서비스(kafka, redis×4, postgres)는 기존과 동일.
애플리케이션 서비스는:
```yaml
gateway:
  image: ghcr.io/gazuua/apex-pipeline-gateway:${SHA_TAG:?SHA_TAG is required}
  volumes:
    - ../../apex_services/tests/e2e/docker/gateway_docker.toml:/app/config.toml:ro
    - ../../apex_services/tests/keys/:/app/keys/:ro
  # ports, depends_on, networks, healthcheck은 e2e.yml과 동일
```

auth-svc, chat-svc도 동일 패턴.

- [ ] **Step 2: Commit**

---

### Task 5: docker-compose.e2e.yml — Docker config volume mount 추가

**Files:**
- Modify: `apex_infra/docker/docker-compose.e2e.yml`

Docker 기반 E2E 실행 시 config 경로 불일치 해결 (BACKLOG-190).

- [ ] **Step 1: 각 서비스에 Docker config volume mount 추가**

```yaml
gateway:
  volumes:
    - ../../apex_services/tests/e2e/docker/gateway_docker.toml:/app/config.toml:ro
    - ../../apex_services/tests/keys/:/app/keys/:ro
```

기존 `../../apex_services/tests/keys/:/app/keys/:ro`는 유지, Docker config를 config.toml로 마운트하여 Dockerfile COPY를 오버라이드.

auth-svc, chat-svc도 동일.

- [ ] **Step 2: Commit**

---

### Task 6: Helm 차트 — Argo Rollouts CRD 지원

**Files:**
- Modify: `apex_infra/k8s/apex-pipeline/charts/apex-common/templates/_helpers.tpl`
- Modify: `apex_infra/k8s/apex-pipeline/charts/gateway/templates/deployment.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/auth-svc/templates/deployment.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/chat-svc/templates/deployment.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/log-svc/templates/deployment.yaml`
- Create: `apex_infra/k8s/apex-pipeline/charts/gateway/templates/rollout.yaml`
- Create: `apex_infra/k8s/apex-pipeline/charts/auth-svc/templates/rollout.yaml`
- Create: `apex_infra/k8s/apex-pipeline/charts/chat-svc/templates/rollout.yaml`
- Create: `apex_infra/k8s/apex-pipeline/charts/log-svc/templates/rollout.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/gateway/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/auth-svc/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/chat-svc/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/charts/log-svc/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/values-prod.yaml`

- [ ] **Step 1: _helpers.tpl — podTemplate 추출**

기존 `apex-common.deployment`에서 `.spec.template` 부분을 `apex-common.podTemplate`으로 추출. deployment와 rollout이 공유.

```
{{- define "apex-common.podTemplate" -}}
metadata:
  annotations:
    checksum/config: {{ include (print $.Template.BasePath "/configmap.yaml") . | sha256sum }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  ... (기존 containers, volumes 등)
{{- end }}
```

`apex-common.deployment`에서 podTemplate을 include하도록 수정.

- [ ] **Step 2: _helpers.tpl — apex-common.rollout 템플릿 추가**

```
{{- define "apex-common.rollout" -}}
{{- if .Values.rollouts.enabled }}
apiVersion: argoproj.io/v1alpha1
kind: Rollout
metadata:
  name: {{ include "apex-common.fullname" . }}
  labels:
    {{- include "apex-common.labels" . | nindent 4 }}
spec:
  replicas: {{ .Values.replicaCount | default 1 }}
  selector:
    matchLabels:
      {{- include "apex-common.selectorLabels" . | nindent 6 }}
  template:
    {{- include "apex-common.podTemplate" . | nindent 4 }}
  strategy:
    canary:
      {{- toYaml .Values.rollouts.canary | nindent 6 }}
{{- end }}
{{- end }}
```

- [ ] **Step 3: 각 서비스 deployment.yaml에 rollouts.enabled 가드 추가**

```yaml
# gateway/templates/deployment.yaml
{{- if not (and .Values.rollouts .Values.rollouts.enabled) }}
{{ include "apex-common.deployment" . }}
{{- end }}
```

auth-svc, chat-svc도 동일.

- [ ] **Step 4: 각 서비스에 rollout.yaml 생성**

```yaml
# gateway/templates/rollout.yaml
{{ include "apex-common.rollout" . }}
```

auth-svc, chat-svc도 동일.

- [ ] **Step 5: values.yaml에 rollouts 기본 설정 추가**

```yaml
# 각 서비스 sub-chart의 values.yaml에 추가
rollouts:
  enabled: false
  canary:
    steps:
      - setWeight: 10
      - pause: {}
      - setWeight: 50
      - pause: {}
      - setWeight: 100
```

umbrella values.yaml에서도 서비스별 오버라이드 가능하도록 구성.

- [ ] **Step 6: helm lint + helm template로 검증**

```bash
cd apex_infra/k8s/apex-pipeline
helm dependency update .
helm lint .
helm lint . -f values-prod.yaml
# 서비스 release 렌더링 (rollouts 비활성)
helm template apex-services . --set kafka.enabled=false ...
# rollouts 활성 렌더링
helm template apex-services . --set kafka.enabled=false --set gateway.rollouts.enabled=true ...
```

- [ ] **Step 7: Commit**

---

### Task 7: ci.yml — bake-services PR 확장 + smoke-test + deploy 잡

**Files:**
- Modify: `.github/workflows/ci.yml`

가장 큰 변경. 3개 잡 추가/수정.

- [ ] **Step 1: bake-services 잡 — PR에서도 실행되도록 조건 변경**

```yaml
if: >-
  always() &&
  needs.build.result == 'success'
```

main 조건 제거. PR에서는 `--set '*.output=type=docker'` + `--set '*.cache-to='`, main에서는 `--push`. IS_MAIN, CMAKE_PRESET 환경변수 추가.

- [ ] **Step 2: smoke-test 잡 추가**

bake-services 성공 후 docker-compose.smoke.yml로 풀스택 기동 + E2E 서브셋 실행.

```yaml
smoke-test:
  permissions:
    contents: read
  needs: [changes, build, bake-services]
  if: always() && github.ref == 'refs/heads/main' && needs.bake-services.result == 'success' && needs.build.result == 'success'
  runs-on: ubuntu-latest
  timeout-minutes: 15
  steps:
    - uses: actions/checkout@v4
    - name: Download E2E binary
      uses: actions/download-artifact@v4
      with:
        name: service-binaries
        path: artifacts
    - name: Make binary executable
      run: chmod +x artifacts/tests/e2e/apex_e2e_tests
    - name: Start full stack
      env:
        SHA_TAG: sha-${{ github.sha }}
      run: |
        docker compose -f apex_infra/docker/docker-compose.smoke.yml up -d \
          --wait --timeout 120
    - name: Run smoke E2E subset
      run: |
        artifacts/tests/e2e/apex_e2e_tests \
          --gtest_filter="*Login*:*SendMessage*" \
          --gtest_output=xml:smoke-results.xml
      env:
        E2E_GATEWAY_HOST: "127.0.0.1"
    - name: Collect logs on failure
      if: failure()
      run: |
        docker compose -f apex_infra/docker/docker-compose.smoke.yml logs > smoke-logs.txt
    - name: Upload logs
      if: failure()
      uses: actions/upload-artifact@v4
      with:
        name: smoke-test-logs
        path: smoke-logs.txt
    - name: Teardown
      if: always()
      run: docker compose -f apex_infra/docker/docker-compose.smoke.yml down -v
```

- [ ] **Step 3: deploy 잡 추가 (main만)**

Argo Rollouts canary 배포 검증.

```yaml
deploy:
  permissions:
    contents: read
    packages: read
  needs: [bake-services, smoke-test, helm-validation]
  if: >-
    always() &&
    github.ref == 'refs/heads/main' &&
    needs.smoke-test.result == 'success' &&
    needs.helm-validation.result == 'success'
  runs-on: ubuntu-latest
  timeout-minutes: 20
  steps:
    - uses: actions/checkout@v4
    - uses: docker/login-action@v3
      with:
        registry: ghcr.io
        username: ${{ github.actor }}
        password: ${{ secrets.GITHUB_TOKEN }}
    - uses: azure/setup-helm@v4
    - name: Start minikube
      uses: medyagh/setup-minikube@v0.0.21
      with:
        memory: '4096'
        cpus: '2'
    - name: Install Argo Rollouts CRD
      run: |
        kubectl apply -f https://github.com/argoproj/argo-rollouts/releases/latest/download/install.yaml
    - name: Wait for Argo Rollouts controller
      run: |
        kubectl wait --for=condition=available deployment/argo-rollouts \
          -n argo-rollouts --timeout=120s
    - name: Load images into minikube
      env:
        SHA_TAG: sha-${{ github.sha }}
      run: |
        for svc in gateway auth-svc chat-svc; do
          docker pull ghcr.io/gazuua/apex-pipeline-$svc:$SHA_TAG
          minikube image load ghcr.io/gazuua/apex-pipeline-$svc:$SHA_TAG
        done
    - name: Helm dependency update
      working-directory: apex_infra/k8s/apex-pipeline
      run: helm dependency update .
    - name: Install infra release
      working-directory: apex_infra/k8s/apex-pipeline
      run: |
        helm upgrade --install apex-infra . \
          -n apex-infra --create-namespace \
          --set gateway.enabled=false \
          --set auth-svc.enabled=false \
          --set chat-svc.enabled=false \
          --set log-svc.enabled=false \
          --wait --timeout 7m
    - name: Install services with Rollouts
      working-directory: apex_infra/k8s/apex-pipeline
      env:
        SHA_TAG: sha-${{ github.sha }}
      run: |
        helm upgrade --install apex-services . \
          -n apex-services --create-namespace \
          --set kafka.enabled=false \
          --set redis-auth.enabled=false \
          --set redis-ratelimit.enabled=false \
          --set redis-chat.enabled=false \
          --set redis-pubsub.enabled=false \
          --set postgresql.enabled=false \
          --set pgbouncer.enabled=false \
          --set namespaces.create=false \
          --set gateway.rollouts.enabled=true \
          --set auth-svc.rollouts.enabled=true \
          --set chat-svc.rollouts.enabled=true \
          --set gateway.serviceMonitor.enabled=false \
          --set auth-svc.serviceMonitor.enabled=false \
          --set chat-svc.serviceMonitor.enabled=false \
          --set gateway.image.tag=$SHA_TAG \
          --set auth-svc.image.tag=$SHA_TAG \
          --set chat-svc.image.tag=$SHA_TAG \
          --wait --timeout 5m
    - name: Verify Rollout resources
      run: |
        kubectl get rollouts -n apex-services
        for svc in gateway auth-svc chat-svc; do
          echo "=== $svc Rollout ==="
          kubectl get rollout -n apex-services -l app.kubernetes.io/name=$svc -o wide || true
        done
    - name: Debug on failure
      if: failure()
      run: |
        echo "=== Argo Rollouts ==="
        kubectl get rollouts -n apex-services -o wide 2>/dev/null || true
        echo "=== Pods ==="
        kubectl get pods -n apex-services -o wide
        for pod in $(kubectl get pods -n apex-services --no-headers -o custom-columns=":metadata.name" 2>/dev/null); do
          echo "=== Logs: $pod ==="
          kubectl logs $pod -n apex-services --tail=30 2>/dev/null || true
        done
```

- [ ] **Step 4: Commit**

---

### Task 8: 최종 검증 + 커밋 정리

- [ ] **Step 1: clang-format** (C++ 소스 변경 없으므로 스킵 가능)
- [ ] **Step 2: 로컬 빌드** (CMakePresets.json 미변경이므로 빌드 영향 없음, 빌드 스킵 조건 충족 여부 확인)
- [ ] **Step 3: PR 생성 + CI 대기**
- [ ] **Step 4: CI 실패 시 수정 반복**
