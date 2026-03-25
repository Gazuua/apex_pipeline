# v0.6.4 CI/CD 고도화 — 설계 스펙

## 개요

v0.6.4 마일스톤: CI에서 서비스 Docker 이미지 빌드/검증/배포까지 완전 자동화.
Argo Rollouts canary 배포 전략 도입, 3단계 검증 파이프라인 확립.

**연결 백로그**: BACKLOG-178 (메인), BACKLOG-147, BACKLOG-190, BACKLOG-217

## 설계 결정 요약

| 항목 | 결정 | 근거 |
|------|------|------|
| CD 범위 | Full CD (Argo Rollouts) | 시각적 배포 관리+모니터링이 운영에 핵심 |
| 배포 전략 | Canary (10→50→100%) | 리소스 효율 + 장애 반경 최소화 |
| 배포 환경 | CI minikube | 로컬 PC 메모리 제약, 클라우드 비용 부담 |
| 알림 채널 | GitHub 자체 (PR comment + commit status) | 1인 개발, 추가 채널 관리 불필요 |
| RSA 키 분리 | 이미 Dockerfile에서 제거됨, 검증만 수행 | volume mount / K8s Secret으로 통일 |
| Docker 이미지 빌드 preset | debug/release 분기 | E2E용 debug, 배포용 release 분리 |
| 스모크 테스트 | Health check + E2E 바이너리 서브셋 | 기존 E2E와 중복 없이 이미지 검증 |
| Argo Rollouts 범위 | Rollout CRD + 수동 승격 | AnalysisTemplate은 Grafana+메트릭 안정화 후 (BACKLOG-232) |

## 3단계 검증 파이프라인

```
e2e (호스트 바이너리)     → 코드가 맞는지 (비즈니스 로직)
smoke-test (docker-compose) → 이미지가 맞는지 (컨테이너 환경)
helm-validation (minikube)  → 배포가 맞는지 (K8s 오케스트레이션)
```

각 단계가 서로 다른 레이어의 문제를 커버하며, 중복 없이 상호 보완.

## CI/CD 파이프라인 구조

```
PR push / main push
    │
    ├─ [기존] format-check
    ├─ [기존] build (5 preset: debug, asan, tsan, ubsan, msvc)
    │    ├─ [기존] e2e (호스트 바이너리, 로직 검증)
    │    ├─ [변경] bake-services
    │    │    ├─ PR: 빌드만 (push 없이 검증, cache read-only)
    │    │    └─ main: 빌드 + GHCR 푸시
    │    │         ├─ [기존] scan (Trivy CRITICAL/HIGH)
    │    │         ├─ [기존] helm-validation (minikube)
    │    │         └─ [신규] smoke-test (docker-compose)
    │    └─ [신규] deploy (main만)
    │         ├─ Argo Rollouts canary 배포 (minikube)
    │         └─ 결과 → commit status
    └─ [기존] go-test
```

**향후 추가 예정 (이번 스코프 외):** tag push 시 semver 태깅 워크플로.

## 세부 설계

### 1. bake-services PR 확장

현재 main 브랜치에서만 실행. PR에서도 이미지 빌드 유효성을 검증하도록 확장.

```yaml
bake-services:
  if: needs.build.result == 'success'  # main 조건 제거
  steps:
    - name: Build service images
      run: |
        if [ "${{ github.ref }}" = "refs/heads/main" ]; then
          docker buildx bake -f docker-bake.hcl services --push
        else
          docker buildx bake -f docker-bake.hcl services \
            --set '*.output=type=docker' \
            --set '*.cache-to='
        fi
```

- PR: `--set '*.output=type=docker'` — 로컬 Docker에 로드만 (push 없음). `cache-to` 비활성화 (쓰기 권한 없음), `cache-from`은 유지 (registry pull read-only)
- main: `--push` — GHCR에 푸시 + 캐시 쓰기 (기존 동작 유지)

### 2. 이미지 태깅 전략 (BACKLOG-217)

docker-bake.hcl의 tags를 조건부로 변경하여 feature 브랜치에서 `:latest` 오염 방지.

**HCL 호환성**: docker buildx bake의 HCL2 방언은 `compact()` 등 Terraform 전용 함수를 지원하지 않음. CI YAML에서 tag 리스트를 조립하여 변수로 전달하는 방식 사용.

```hcl
// docker-bake.hcl
variable "IS_MAIN" { default = "false" }

// 각 서비스 target의 tags — CI에서 IS_MAIN에 따라 분기
// IS_MAIN="true"일 때만 latest 포함
```

CI에서의 호출:
```yaml
# main
docker buildx bake ... \
  --set '*.args.CI_IMAGE_TAG=${{ env.CI_TAG }}' \
  SHA_TAG=sha-${GITHUB_SHA::7} IS_MAIN=true

# PR / feature
docker buildx bake ... \
  SHA_TAG=sha-${GITHUB_SHA::7} IS_MAIN=false
```

**태깅 결과:**
- feature 브랜치: `sha-xxx`만
- main: `sha-xxx` + `latest`
- git tag push: semver 태깅은 향후 별도 워크플로에서 구현

**구현 방식**: 각 서비스 target의 tags에서 삼항 연산자 사용. docker buildx bake HCL2는 삼항(`condition ? a : b`)과 `notequal()`을 지원.

```hcl
target "gateway" {
  inherits = ["service-base"]
  tags = notequal(IS_MAIN, "true") ? [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
  ] : [
    "${REGISTRY}/apex-pipeline-gateway:${SHA_TAG}",
    "${REGISTRY}/apex-pipeline-gateway:latest",
  ]
  // ...
}
```

### 3. Docker 이미지 빌드 preset 분리

**문제**: 현재 service.Dockerfile이 `cmake --preset debug`로 빌드. 최적화 없음, 디버그 심볼 포함, assert 활성화. E2E 테스트용으로는 적합하나 배포용으로 부적절.

**해결**: `CMAKE_PRESET` 빌드 인자 추가로 debug/release 분기.

```dockerfile
# service.Dockerfile
ARG CMAKE_PRESET=debug
RUN --mount=type=cache,target=/root/.cache/sccache \
    cmake --preset ${CMAKE_PRESET} -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
    && cmake --build build/Linux/${CMAKE_PRESET} --target ${CMAKE_TARGET} \
    && mkdir -p /out \
    && cp build/Linux/${CMAKE_PRESET}/apex_services/${SERVICE_DIR}/${CMAKE_TARGET} /out/${CMAKE_TARGET}
```

docker-bake.hcl에 preset 변수 추가:
```hcl
variable "CMAKE_PRESET" { default = "debug" }

target "service-base" {
  args = {
    CMAKE_PRESET = CMAKE_PRESET
  }
}
```

**CI에서의 사용**:
- E2E / 스모크 테스트: `CMAKE_PRESET=debug` (기본값)
- bake-services (GHCR 푸시): `CMAKE_PRESET=release` — 배포용 이미지
- docker-compose.smoke.yml: release 이미지를 사용하여 배포 이미지 검증

**전제조건**: CMakePresets.json에 Linux용 `release` preset이 필요. 현재 `debug`, `asan`, `tsan`, `ubsan` preset만 존재하므로 `release` preset 추가 필수 (RelWithDebInfo 또는 Release).

### 4. RSA 키 확인 (BACKLOG-147)

**현재 상태 확인 결과: service.Dockerfile에 RSA 키 COPY가 이미 없음.**

runtime 스테이지의 COPY는:
- 빌드 바이너리: `COPY --from=builder /out/${CMAKE_TARGET} /app/${CMAKE_TARGET}`
- E2E config: `COPY apex_services/tests/e2e/${CONFIG_FILE} /app/config.toml`

**RSA 키 파일은 이미지에 포함되지 않음.** 키 주입은 환경별로:
- docker-compose E2E: volume mount (`../../apex_services/tests/keys/:/app/keys/:ro`)
- K8s: Secret 볼륨 마운트 (`/app/keys/`)

**이번 작업**: Dockerfile 변경 없음. BACKLOG-147은 코드 검증 후 resolve 처리.

### 4. RSA 키 경로 정합성 (BACKLOG-190)

**문제 상세:**
- E2E config (이미지에 baked): `private_key_file = "apex_services/tests/keys/test_rs256.pem"` (호스트 상대 경로)
- 컨테이너 CWD: `/app/`
- 컨테이너에서 해석: `/app/apex_services/tests/keys/test_rs256.pem` → 존재하지 않음
- volume mount: `/app/keys/test_rs256.pem` → 경로 불일치

**호스트 E2E (CI)에서는 문제없음**: 서비스가 repo root에서 실행되므로 상대 경로가 정상 동작.
**Docker E2E에서만 발생**: 컨테이너 내부 경로 체계가 다름.

**해결 방안: Docker 전용 config 파일 세트 생성**

```
신규 파일:
  apex_services/tests/e2e/docker/gateway_docker.toml
  apex_services/tests/e2e/docker/auth_svc_docker.toml
  apex_services/tests/e2e/docker/chat_svc_docker.toml
```

Docker config에서 변경하는 항목:

1. **키 경로**: 호스트 상대 경로 → 컨테이너 내부 경로
```toml
# 호스트용 (기존): private_key_file = "apex_services/tests/keys/test_rs256.pem"
# Docker용 (신규): private_key_file = "keys/test_rs256.pem"
```

2. **인프라 엔드포인트**: localhost → 컨테이너 이름, 매핑 포트 → 내부 포트
```toml
# 호스트용: brokers = "localhost:9092", host = "localhost", port = 6380
# Docker용: brokers = "kafka:9092", host = "redis-auth", port = 6379
```

3. **메트릭 섹션 추가** (스모크 테스트 health check용, Gateway config)
```toml
[metrics]
enabled = true
port = 8081
```

docker-compose.e2e.yml에서 config + 키를 volume mount:
```yaml
gateway:
  volumes:
    - ../../apex_services/tests/e2e/docker/gateway_docker.toml:/app/config.toml:ro
    - ../../apex_services/tests/keys/:/app/keys/:ro
```

이렇게 하면 Dockerfile의 `COPY ${CONFIG_FILE} /app/config.toml`은 빌드 시 기본값으로 bake되지만, docker-compose volume mount가 런타임에 오버라이드.

### 5. docker-compose 스모크 테스트

bake-services로 빌드된 이미지를 docker-compose로 기동하여 통합 검증.

**핵심 설계 포인트:**
1. **별도 docker-compose 파일 필요**: 기존 `docker-compose.e2e.yml`은 `build:` 블록으로 소스 빌드. 스모크 테스트는 **사전 빌드된 GHCR 이미지**를 사용해야 하므로 `image:` 블록으로 교체한 전용 compose 파일 필요.
2. **서비스가 WebSocket/TCP 기반**: curl로 비즈니스 시나리오 불가. 기존 `apex_e2e_tests` 바이너리를 활용.

```yaml
# docker-compose.smoke.yml (신규)
services:
  gateway:
    image: ghcr.io/gazuua/apex-pipeline-gateway:${SHA_TAG}
    volumes:
      - ../../apex_services/tests/e2e/docker/gateway_docker.toml:/app/config.toml:ro
      - ../../apex_services/tests/keys/:/app/keys/:ro
    # ... (인프라 depends_on, 포트, 네트워크는 e2e.yml과 동일)
  # auth-svc, chat-svc도 동일 패턴
```

**CI 잡 구조:**
```yaml
smoke-test:
  needs: [changes, build, bake-services]
  if: github.ref == 'refs/heads/main' && needs.bake-services.result == 'success' && needs.build.result == 'success'
  steps:
    - name: Start full stack (pre-built images)
      run: |
        docker compose -f apex_infra/docker/docker-compose.smoke.yml up -d \
          --wait --timeout 120
      env:
        SHA_TAG: sha-${{ github.sha }}

    - name: Health check
      run: |
        # metrics 포트(8081)의 health endpoint 확인
        curl -sf http://localhost:8081/health || exit 1

    - name: Run smoke E2E subset
      run: |
        # 기존 E2E 바이너리로 핵심 경로만 실행
        artifacts/tests/e2e/apex_e2e_tests \
          --gtest_filter="*Login*:*SendMessage*"

    - name: Collect logs on failure
      if: failure()
      run: docker compose -f apex_infra/docker/docker-compose.smoke.yml logs > smoke-logs.txt

    - name: Upload logs artifact
      if: failure()
      uses: actions/upload-artifact@v4
      with:
        name: smoke-test-logs
        path: smoke-logs.txt

    - name: Teardown
      if: always()
      run: docker compose -f apex_infra/docker/docker-compose.smoke.yml down -v
```

**E2E 바이너리는 호스트에서 실행, 서비스는 Docker**:
- 이미 CI E2E와 동일한 패턴 (호스트 바이너리 → Docker 인프라)
- 차이점: 서비스도 Docker 이미지에서 실행 → 이미지 유효성 검증

### 6. Argo Rollouts canary 배포

#### 6-1. Argo Rollouts 설치

deploy 잡에서 minikube에 Argo Rollouts Controller CRD + Controller 설치.

```yaml
- name: Install Argo Rollouts CRD
  run: |
    kubectl apply -f https://github.com/argoproj/argo-rollouts/releases/latest/download/install.yaml

- name: Wait for Argo Rollouts controller
  run: |
    kubectl wait --for=condition=available deployment/argo-rollouts \
      -n argo-rollouts --timeout=120s
```

> Helm chart 대신 raw manifest 사용 -- CI 환경에서 Helm repo 추가 없이 빠르게 설치 가능. Dashboard는 CI에서 불필요하므로 미포함.

#### 6-2. Deployment → Rollout CRD 전환

Helm 차트에서 조건부로 Deployment 또는 Rollout을 생성.

```yaml
# values.yaml 추가
rollouts:
  enabled: false  # 기본값: 일반 Deployment
  canary:
    steps:
      - setWeight: 10
      - pause: {}         # 수동 승격 대기
      - setWeight: 50
      - pause: {}
      - setWeight: 100
```

**Deployment/Rollout 배타적 제어 메커니즘:**

각 서비스 sub-chart의 기존 `deployment.yaml`에 조건 가드 추가 (nil-safe 체크):
```yaml
# gateway/templates/deployment.yaml (기존 파일 수정, auth-svc/chat-svc/log-svc도 동일)
{{- if not (and .Values.rollouts .Values.rollouts.enabled) }}
{{ include "apex-common.deployment" . }}
{{- end }}
```

각 서비스 sub-chart에 `rollout.yaml` 생성 (named template 호출만):
```yaml
# gateway/templates/rollout.yaml (신규, auth-svc/chat-svc/log-svc도 동일)
{{ include "apex-common.rollout" . }}
```

`apex-common.rollout` named template이 `rollouts.enabled` 가드를 내장:
```yaml
# _helpers.tpl 내 apex-common.rollout
{{- if and .Values.rollouts .Values.rollouts.enabled }}
apiVersion: argoproj.io/v1alpha1
kind: Rollout
...
{{- end }}
```

**`_helpers.tpl` 수정 범위**:
- `apex-common.podTemplate` 네임드 템플릿 추출 — deployment.yaml과 rollout.yaml이 Pod spec을 공유하도록 리팩터링
- 기존 `apex-common.deployment`에서 podTemplate 부분을 분리
- `apex-common.rollout` 네임드 템플릿 추가 — 가드 + Rollout CRD 렌더링 내장

**동시 존재 방지**: `rollouts.enabled`의 if/else로 한쪽만 렌더링. 같은 이름의 Deployment와 Rollout이 동시에 생성되지 않음.

#### 6-3. CI에서 canary 검증

deploy 잡은 helm-validation과 **별도 minikube 인스턴스** 사용 (GitHub Actions job은 각각 독립 runner).

```yaml
deploy:
  needs: [bake-services, smoke-test, helm-validation]
  if: github.ref == 'refs/heads/main'
  steps:
    - name: Setup minikube
      run: minikube start --memory=4096

    - name: Install Argo Rollouts CRD
      run: |
        kubectl apply -f https://github.com/argoproj/argo-rollouts/releases/latest/download/install.yaml

    - name: Wait for Argo Rollouts controller
      run: |
        kubectl wait --for=condition=available deployment/argo-rollouts \
          -n argo-rollouts --timeout=120s

    - name: Deploy with Rollouts
      run: |
        # 2-release 모델 유지: infra(Bitnami) → services(Rollout CRD)
        # helm-validation 잡과 동일한 패턴이나 rollouts.enabled=true 추가
        helm install apex-infra ./apex_infra/k8s/apex-pipeline \
          --namespace apex-infra --create-namespace \
          --set services.enabled=false
        helm install apex-services ./apex_infra/k8s/apex-pipeline \
          --namespace apex-services --create-namespace \
          --set infrastructure.enabled=false \
          --set rollouts.enabled=true \
          --set global.image.tag=sha-${{ github.sha }}

    - name: Verify Rollout healthy
      run: |
        kubectl argo rollouts status apex-gateway --timeout 120s
        kubectl argo rollouts get rollout apex-gateway

    - name: Verify canary steps configured
      run: |
        # Rollout CRD가 canary steps를 올바르게 갖고 있는지 확인
        kubectl get rollout apex-gateway -o jsonpath='{.spec.strategy.canary.steps}' \
          | grep -q "setWeight"
```

**canary 진행 검증 전략**: 초기 배포의 **Rollout 리소스 정상 생성 + 안정화**를 검증. 실제 canary 트래픽 분배 테스트(이미지 변경 → 단계적 승격)는 Argo Rollouts 컨트롤러의 검증 영역이므로, 우리 Helm 차트가 올바른 Rollout CRD를 생성하는지에 집중.

### 7. 알림

GitHub 네이티브 알림만 사용:
- **commit status**: 각 잡의 성공/실패가 자동으로 commit status에 반영 (GitHub Actions 기본 동작)
- **PR comment**: 스모크 테스트 실패 시 서비스 로그 요약을 PR comment로 게시

별도 Slack/Discord 웹훅은 미구현 (1인 개발 환경에서 불필요).

## 변경 파일 목록

```
수정:
  .github/workflows/ci.yml                              — smoke-test/deploy 잡 추가, bake-services PR 확장
  apex_infra/docker/service.Dockerfile                   — CMAKE_PRESET 빌드 인자 추가 (debug/release 분기)
  apex_infra/docker/docker-bake.hcl                      — IS_MAIN, CMAKE_PRESET 변수, latest 태그 조건부
  apex_infra/docker/docker-compose.e2e.yml               — Docker config volume mount 추가
  apex_infra/k8s/apex-pipeline/values.yaml               — 서비스별 rollouts 섹션 추가
  apex_infra/k8s/apex-pipeline/values-prod.yaml          — rollouts 관련 주석 추가
  apex_infra/k8s/apex-pipeline/charts/apex-common/templates/_helpers.tpl  — podTemplate 추출 + rollout 템플릿 추가
  apex_infra/k8s/apex-pipeline/charts/gateway/templates/deployment.yaml   — rollouts.enabled 가드
  apex_infra/k8s/apex-pipeline/charts/auth-svc/templates/deployment.yaml  — rollouts.enabled 가드
  apex_infra/k8s/apex-pipeline/charts/chat-svc/templates/deployment.yaml  — rollouts.enabled 가드
  apex_infra/k8s/apex-pipeline/charts/log-svc/templates/deployment.yaml   — rollouts.enabled 가드
  apex_infra/k8s/apex-pipeline/charts/gateway/values.yaml   — rollouts 기본 설정 추가
  apex_infra/k8s/apex-pipeline/charts/auth-svc/values.yaml  — rollouts 기본 설정 추가
  apex_infra/k8s/apex-pipeline/charts/chat-svc/values.yaml  — rollouts 기본 설정 추가
  apex_infra/k8s/apex-pipeline/charts/log-svc/values.yaml   — rollouts 기본 설정 추가

신규:
  apex_infra/docker/docker-compose.smoke.yml             — 스모크 테스트용 (pre-built image 사용)
  apex_services/tests/e2e/docker/gateway_docker.toml     — Docker용 Gateway config
  apex_services/tests/e2e/docker/auth_svc_docker.toml    — Docker용 Auth config
  apex_services/tests/e2e/docker/chat_svc_docker.toml    — Docker용 Chat config
  apex_infra/k8s/apex-pipeline/charts/gateway/templates/rollout.yaml   — Rollout CRD 인클루드
  apex_infra/k8s/apex-pipeline/charts/auth-svc/templates/rollout.yaml  — Rollout CRD 인클루드
  apex_infra/k8s/apex-pipeline/charts/chat-svc/templates/rollout.yaml  — Rollout CRD 인클루드
  apex_infra/k8s/apex-pipeline/charts/log-svc/templates/rollout.yaml   — Rollout CRD 인클루드
```

**변경 없음 (확인 완료):**
- `apex_infra/docker/service.Dockerfile` — RSA 키 COPY 이미 없음 (BACKLOG-147)
- `CMakePresets.json` — Linux release preset이 이미 존재 (추가 불필요)

## 향후 확장 (이번 스코프 외 — 별도 마일스톤에서 진행)

- BACKLOG-232: Grafana + 메트릭 안정화 → AnalysisTemplate 자동 승격 (Argo Rollouts 자동 판정 전제조건)
- Self-hosted runner 전환 시 deploy 잡의 타겟만 변경
- Slack/Discord 알림 추가 (팀 확장 시)
- Semver 자동 태깅 (tag push 트리거 워크플로)
