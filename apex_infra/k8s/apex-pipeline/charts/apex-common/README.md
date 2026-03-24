# apex-common — Helm Library Chart

Apex Pipeline 서비스 sub-chart들이 공유하는 **공통 Helm 템플릿 라이브러리**.

## Library Chart란?

일반 Helm chart(`type: application`)는 K8s 리소스를 직접 생성한다. Library chart(`type: library`)는 **자체로는 아무 리소스도 생성하지 않고**, 다른 chart가 가져다 쓸 수 있는 named template만 제공한다.

Go의 interface, C++의 header-only library와 비슷한 개념이다. "뼈대는 공통, 값은 각자" 패턴.

## 사용법

### 1. sub-chart에서 의존성 선언

```yaml
# charts/gateway/Chart.yaml
dependencies:
  - name: apex-common
    version: "0.1.0"
    repository: "file://../apex-common"
```

### 2. 템플릿에서 include로 호출

```yaml
# charts/gateway/templates/deployment.yaml
{{- include "apex-common.deployment" . }}
```

이 한 줄이 `gateway/values.yaml`의 값을 읽어 완전한 Deployment YAML을 생성한다.

## 제공 Named Templates

| Template | 역할 | 사용 예 |
|----------|------|--------|
| `apex-common.name` | Chart 이름 (truncated 63자) | 내부 헬퍼 |
| `apex-common.fullname` | Release-Chart 풀네임 | 모든 리소스 metadata.name |
| `apex-common.labels` | 표준 라벨셋 (app.kubernetes.io/*) | 모든 리소스 metadata.labels |
| `apex-common.selectorLabels` | matchLabels | Deployment.spec.selector, Service.spec.selector |
| `apex-common.deployment` | **Deployment 전체** | `templates/deployment.yaml` |
| `apex-common.service` | **Service 전체** | `templates/service.yaml` |
| `apex-common.configmap` | **ConfigMap** (TOML 임베딩) | `templates/configmap.yaml` |
| `apex-common.serviceMonitor` | Prometheus ServiceMonitor CRD | `templates/service-monitor.yaml` |
| `apex-common.serviceAccount` | ServiceAccount | `templates/service-account.yaml` |

## values.yaml 스키마

`apex-common.deployment`가 기대하는 values 구조:

```yaml
# === 필수 ===
replicaCount: 1

image:
  repository: ghcr.io/gazuua/apex-pipeline-<service>
  tag: "latest"
  pullPolicy: IfNotPresent

service:
  type: ClusterIP
  ports:
    - name: metrics          # 포트 이름 (probe, ServiceMonitor에서 참조)
      port: 8082             # Service가 노출하는 포트
      targetPort: 8082       # 컨테이너 내부 포트 (생략 시 port와 동일)

config:
  fileName: "config.toml"   # ConfigMap에 마운트될 파일명
  mountPath: "/app"          # 컨테이너 내 마운트 경로
  content: |                 # TOML 설정 내용
    [logging]
    service_name = "my-service"

# === 프로브 (3종) ===
probes:
  startup:                   # 콜드 스타트 보호
    httpGet:
      path: /health
      port: metrics
    failureThreshold: 30
    periodSeconds: 5
  liveness:                  # 프로세스 행 감지
    httpGet:
      path: /health
      port: metrics
    initialDelaySeconds: 10
    periodSeconds: 15
  readiness:                 # 트래픽 수신 준비
    httpGet:
      path: /ready
      port: metrics
    initialDelaySeconds: 5
    periodSeconds: 5

# === 선택 ===
resources:
  requests:
    cpu: 100m
    memory: 128Mi
  limits:
    cpu: 500m
    memory: 256Mi

secrets:
  existingSecret: ""         # 외부 Secret 이름 (비어있으면 자동 생성)

extraVolumes: []             # 추가 볼륨 (RSA 키 등)
extraVolumeMounts: []        # 추가 볼륨 마운트
extraEnv: []                 # 추가 환경변수

serviceAccount:
  create: false
  name: ""
  annotations: {}

serviceMonitor:
  enabled: true
  port: metrics
  path: /metrics
  interval: 15s
```

## 새 서비스 추가 방법

### Step 1: sub-chart 디렉토리 생성

```bash
mkdir -p charts/my-new-svc/templates/tests
```

### Step 2: Chart.yaml

```yaml
apiVersion: v2
name: my-new-svc
description: My New Service
type: application
version: 0.1.0
appVersion: "0.6.3"
dependencies:
  - name: apex-common
    version: "0.1.0"
    repository: "file://../apex-common"
```

### Step 3: values.yaml

위 스키마를 복사하고 서비스에 맞게 수정:
- `image.repository` 변경
- `service.ports` 변경 (metrics 포트)
- `config.content`에 서비스 TOML 작성
- 필요하면 `extraVolumes`/`extraEnv` 추가

### Step 4: templates (최소 3파일)

```yaml
# templates/deployment.yaml
{{- include "apex-common.deployment" . }}

# templates/service.yaml
{{- include "apex-common.service" . }}

# templates/configmap.yaml
{{- include "apex-common.configmap" . }}
```

### Step 5: umbrella Chart.yaml에 의존성 추가

```yaml
# apex-pipeline/Chart.yaml
dependencies:
  - name: my-new-svc
    version: "0.1.0"
    condition: my-new-svc.enabled
```

### Step 6: helm dependency update + lint

```bash
cd apex_infra/k8s/apex-pipeline
helm dependency update .
helm lint .
```

## 커스터마이징 포인트

### Ingress / HPA / PDB 추가 (Gateway 패턴)

Library chart에 포함하지 않는 서비스별 전용 리소스는 sub-chart에 직접 추가:

```yaml
# charts/my-new-svc/templates/ingress.yaml
{{- if .Values.ingress.enabled }}
apiVersion: networking.k8s.io/v1
kind: Ingress
# ... 서비스별 Ingress 정의
{{- end }}
```

### extraVolumes 예시 (RSA 키 마운트)

```yaml
# values.yaml
extraVolumes:
  - name: rsa-keys
    secret:
      secretName: apex-rsa-keys

extraVolumeMounts:
  - name: rsa-keys
    mountPath: /app/keys
    readOnly: true
```

### extraEnv 예시

```yaml
extraEnv:
  - name: LOG_LEVEL
    value: "debug"
  - name: CUSTOM_VAR
    valueFrom:
      secretKeyRef:
        name: my-secret
        key: value
```

## ConfigMap 변경 감지

Deployment 템플릿에 `checksum/config` annotation이 포함되어 있다. ConfigMap 내용이 변경되면 체크섬이 바뀌어 Pod이 자동으로 재시작된다. 이는 설정 변경이 반영되지 않는 문제를 방지한다.
