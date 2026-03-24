# Apex Pipeline Helm Chart

`helm install` 한 방으로 Apex Pipeline 전체 스택(Gateway + Auth/Chat/Log 서비스 + Kafka/Redis/PostgreSQL 인프라)을 Kubernetes에 배포하는 umbrella Helm chart.

## 목차

1. [개요](#1-개요)
2. [전제 조건](#2-전제-조건)
3. [빠른 시작](#3-빠른-시작)
4. [차트 구조](#4-차트-구조)
5. [2-Release 배포 전략](#5-2-release-배포-전략)
6. [환경 분리](#6-환경-분리)
7. [values 커스터마이징](#7-values-커스터마이징)
8. [새 서비스 추가](#8-새-서비스-추가)
9. [시크릿 관리](#9-시크릿-관리)
10. [Ingress 설정](#10-ingress-설정)
11. [트러블슈팅](#11-트러블슈팅)
12. [명령어 레퍼런스](#12-명령어-레퍼런스)

---

## 1. 개요

이 차트는 Apex Pipeline의 모든 구성 요소를 Kubernetes 위에 선언적으로 배포한다.

**구성 요소**:

| 구분 | 컴포넌트 | 비고 |
|------|----------|------|
| 앱 서비스 | Gateway, Auth, Chat, Log | C++23 코루틴 기반 서버 |
| 메시징 | Kafka (KRaft) | 서비스 간 비동기 통신 |
| 캐시 | Redis x4 (auth, ratelimit, chat, pubsub) | 용도별 인스턴스 분리 |
| DB | PostgreSQL + PgBouncer | 커넥션 풀링 포함 |

**핵심 설계**:
- **Library chart** (`apex-common`) — 서비스 sub-chart의 Deployment/Service/ConfigMap 보일러플레이트를 80% 재사용
- **2-Release 배포** — 인프라와 앱 서비스를 별도 namespace/release로 라이프사이클 분리
- **expand_env 패턴** — TOML 설정에 `${VAR:-default}` 패턴을 쓰고, K8s Secret을 환경변수로 주입하면 서비스 코드가 런타임에 자동 치환. 서비스 코드 수정 없음

---

## 2. 전제 조건

| 도구 | 최소 버전 | 설치 확인 |
|------|-----------|-----------|
| **minikube** | v1.32+ | `minikube version` |
| **Helm** | v3.14+ | `helm version` |
| **kubectl** | v1.29+ (K8s 버전과 +-1 호환) | `kubectl version --client` |
| **Docker Desktop** 또는 **Hyper-V** | minikube 드라이버 | `docker info` |

minikube 권장 리소스: CPU 2코어, 메모리 4GB 이상 (`local-setup.sh`가 자동 설정).

---

## 3. 빠른 시작

### 원클릭 설치 (로컬)

```bash
cd apex_infra/k8s/apex-pipeline
./scripts/local-setup.sh
```

이 스크립트가 자동으로 수행하는 단계:

1. minikube 시작 (`--memory=4096 --cpus=2`)
2. Ingress addon 활성화
3. Helm dependency update (Bitnami chart 다운로드)
4. apex-infra release 설치 (Kafka, Redis, PostgreSQL, PgBouncer)
5. apex-services release 설치 (Gateway, Auth, Chat)
6. `helm test` 실행 (각 서비스 연결 확인)

설치 완료 후 별도 터미널에서:

```bash
minikube tunnel
```

이후 `wss://apex.local`로 접근 가능하다.

### 정리

```bash
./scripts/local-teardown.sh
```

---

## 4. 차트 구조

```
apex_infra/k8s/apex-pipeline/
├── Chart.yaml                  # umbrella chart 정의 (의존성 목록)
├── Chart.lock                  # 의존성 버전 고정 (자동 생성)
├── values.yaml                 # 로컬(minikube) 기본값
├── values-prod.yaml            # 프로덕션 오버라이드
├── .helmignore                 # Helm 패키징 시 제외 목록
├── charts/                     # sub-charts
│   ├── apex-common/            # library chart (공통 템플릿)
│   │   ├── Chart.yaml          #   type: library
│   │   ├── README.md           #   사용법 가이드
│   │   └── templates/
│   │       └── _helpers.tpl    #   named templates (deployment, service 등)
│   ├── gateway/                # Gateway sub-chart
│   │   ├── Chart.yaml          #   apex-common 의존
│   │   ├── values.yaml         #   기본값 (포트, TOML, 프로브 등)
│   │   └── templates/
│   │       ├── deployment.yaml #   apex-common.deployment include
│   │       ├── service.yaml    #   apex-common.service include
│   │       ├── configmap.yaml  #   apex-common.configmap include
│   │       ├── ingress.yaml    #   Gateway 전용 (nginx, WebSocket)
│   │       ├── hpa.yaml        #   Gateway 전용 (수평 오토스케일링)
│   │       ├── pdb.yaml        #   Gateway 전용 (PodDisruptionBudget)
│   │       ├── service-monitor.yaml
│   │       └── tests/
│   │           └── test-connection.yaml
│   ├── auth-svc/               # Auth 서비스 (동일 패턴)
│   ├── chat-svc/               # Chat 서비스 (동일 패턴)
│   ├── log-svc/                # Log 서비스 (enabled: false 기본)
│   └── pgbouncer/              # PgBouncer (로컬 sub-chart, Bitnami Docker 이미지)
├── templates/                  # umbrella 레벨 리소스
│   ├── namespaces.yaml         #   apex-infra, apex-services namespace 생성
│   └── secrets.yaml            #   공통 Secret (RSA 키 등)
└── scripts/
    ├── local-setup.sh          #   minikube 원클릭 셋업
    └── local-teardown.sh       #   정리
```

### 파일별 역할

| 파일 | 역할 |
|------|------|
| `Chart.yaml` | umbrella chart 메타데이터 + 모든 sub-chart/Bitnami 의존성 선언. `condition` 필드로 개별 토글 |
| `values.yaml` | 로컬 환경 기본값. 인프라 sub-chart 전부 활성, 개발용 시크릿 하드코딩 |
| `values-prod.yaml` | 프로덕션 오버라이드. 인프라 비활성, `existingSecret` 참조, HPA/PDB 활성 |
| `charts/apex-common/` | library chart. 자체 리소스를 생성하지 않고 named template만 제공. 모든 서비스 sub-chart가 이걸 `include`해서 Deployment/Service/ConfigMap을 렌더링 |
| `charts/<service>/values.yaml` | 각 서비스의 이미지, 포트, TOML 설정, 프로브, 리소스 기본값 |
| `templates/secrets.yaml` | RSA 키 Secret. `rsaKeys.create: true`일 때만 생성 (로컬) |

### 의존성 구조

```
apex-pipeline (umbrella)
├── gateway          ← apex-common (library)
├── auth-svc         ← apex-common
├── chat-svc         ← apex-common
├── log-svc          ← apex-common
├── kafka            ← bitnami/kafka ~32.x
├── redis-auth       ← bitnami/redis ~21.x (alias)
├── redis-ratelimit  ← bitnami/redis ~21.x (alias)
├── redis-chat       ← bitnami/redis ~21.x (alias)
├── redis-pubsub     ← bitnami/redis ~21.x (alias)
├── postgresql       ← bitnami/postgresql ~16.x
└── pgbouncer        ← 로컬 sub-chart (Bitnami Docker 이미지)
```

Redis 4개는 같은 bitnami/redis chart를 alias로 분리한 것이다. 각각 독립 인스턴스로 배포되며 설정도 별개다.

---

## 5. 2-Release 배포 전략

### 왜 2-Release인가?

Helm은 하나의 release를 하나의 namespace에 배포하는 것이 표준 동작이다. 인프라(Kafka, Redis, PostgreSQL)와 앱 서비스(Gateway, Auth, Chat, Log)를 다른 namespace에 배포하려면, 모든 템플릿에 namespace를 하드코딩하거나 Bitnami chart의 `namespaceOverride`를 써야 하는데 둘 다 번거롭다.

대신 **같은 chart를 두 번 설치**하되, `--set`으로 각 release에 필요 없는 컴포넌트를 끈다.

### Release 1: 인프라 (`apex-infra` namespace)

```bash
helm upgrade --install apex-infra . \
  -n apex-infra --create-namespace \
  --set gateway.enabled=false \
  --set auth-svc.enabled=false \
  --set chat-svc.enabled=false \
  --set log-svc.enabled=false \
  --wait --timeout 5m
```

이 release에서 활성화되는 것: Kafka, Redis x4, PostgreSQL, PgBouncer.

### Release 2: 서비스 (`apex-services` namespace)

```bash
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
```

이 release에서 활성화되는 것: Gateway, Auth, Chat (Log는 `enabled: false` 기본).

### 크로스 namespace 통신

서비스가 인프라에 접근할 때는 K8s FQDN을 사용한다:

```
<release-name>-<service>.apex-infra.svc.cluster.local
```

예: `apex-infra-kafka.apex-infra.svc.cluster.local:9092`

이 주소는 각 서비스의 TOML ConfigMap에 이미 설정되어 있다.

### 프로덕션에서는?

인프라 release를 설치하지 않는다. 관리형 서비스(AWS MSK, ElastiCache, RDS 등)를 사용하고, 엔드포인트만 `values-prod.yaml`에서 지정한다.

---

## 6. 환경 분리

### values.yaml (로컬/minikube)

| 항목 | 값 | 이유 |
|------|-----|------|
| 인프라 sub-chart | `enabled: true` | 로컬에서 Bitnami로 직접 실행 |
| 시크릿 | 평문 하드코딩 (`dev_redis_pass`) | 개발 편의 |
| RSA 키 | `rsaKeys.create: true` (테스트 키 내장) | 로컬 JWT 검증 |
| Ingress host | `apex.local` | minikube tunnel 사용 |
| HPA/PDB | `enabled: false` | 단일 Pod 충분 |
| replicas | 전부 1 | 리소스 절약 |
| resources | 최소값 (100m CPU, 128Mi 메모리) | minikube 제한 |

### values-prod.yaml (프로덕션)

```bash
helm install apex-services . -n apex-services -f values.yaml -f values-prod.yaml
```

`-f` 순서에 따라 뒤의 파일이 앞의 값을 오버라이드한다.

| 항목 | 값 | 이유 |
|------|-----|------|
| 인프라 sub-chart | `enabled: false` | 관리형 서비스 사용 |
| 시크릿 | `existingSecret` 참조 | 운영팀이 사전 생성 |
| RSA 키 | `rsaKeys.existingSecret` 참조 | 보안 관리 |
| Ingress host | `apex.example.com` | 실제 도메인 |
| HPA | `enabled: true` (min:1, max:10) | 트래픽 대응 |
| PDB | `enabled: true` (maxUnavailable:1) | 가용성 보장 |
| resources | 프로덕션 사이징 (500m~2000m CPU) | 안정 운영 |

---

## 7. values 커스터마이징

### 서비스 토글 (`enabled`)

모든 서비스와 인프라 컴포넌트는 `condition` 필드로 개별 활성화/비활성화할 수 있다:

```yaml
gateway:
  enabled: true     # Gateway 배포
log-svc:
  enabled: false    # Log 서비스 스킵 (바이너리 미구현)
kafka:
  enabled: true     # Bitnami Kafka 배포
```

### 이미지 태그 (`image.tag`)

각 서비스의 컨테이너 이미지를 지정한다:

```yaml
gateway:
  image:
    repository: ghcr.io/gazuua/apex-pipeline-gateway
    tag: "v0.6.3"         # 기본값: "latest"
    pullPolicy: IfNotPresent
```

### 시크릿 (`secrets`)

로컬에서는 `secrets.data`에 평문으로, 프로덕션에서는 `secrets.existingSecret`으로:

```yaml
# 로컬
gateway:
  secrets:
    data:
      REDIS_AUTH_PASSWORD: dev_redis_pass

# 프로덕션
gateway:
  secrets:
    existingSecret: "apex-gateway-secrets"
```

### Ingress

Gateway 전용. WebSocket 연결을 외부로 노출한다:

```yaml
gateway:
  ingress:
    enabled: true
    className: nginx
    host: apex.local
    tls:
      enabled: true
      secretName: apex-tls
    annotations:
      nginx.ingress.kubernetes.io/proxy-read-timeout: "3600"
```

### HPA (Horizontal Pod Autoscaler)

Gateway 전용. CPU 사용률 기반 자동 스케일링:

```yaml
gateway:
  hpa:
    enabled: true           # 로컬: false, 프로덕션: true
    minReplicas: 1
    maxReplicas: 10
    targetCPUUtilizationPercentage: 70
```

### PDB (PodDisruptionBudget)

Gateway 전용. 노드 드레인 시 최소 가용 Pod 보장:

```yaml
gateway:
  pdb:
    enabled: true           # 로컬: false, 프로덕션: true
    maxUnavailable: 1
```

### 리소스 (`resources`)

```yaml
auth-svc:
  resources:
    requests:
      cpu: 100m             # 최소 보장
      memory: 128Mi
    limits:
      cpu: 500m             # 최대 허용
      memory: 256Mi
```

### 프로브 (`probes`)

3종 프로브가 모든 서비스에 적용된다:

| 프로브 | 역할 | 기본 설정 |
|--------|------|-----------|
| `startup` | 콜드 스타트 보호 (Kafka/Redis 연결 대기) | 최대 150초 (30회 x 5초) |
| `liveness` | 프로세스 행 감지 -> 자동 재시작 | 10초 후 시작, 15초 간격 |
| `readiness` | 트래픽 수신 준비 확인 | 5초 후 시작, 5초 간격 |

```yaml
auth-svc:
  probes:
    startup:
      httpGet:
        path: /health
        port: 8082
      failureThreshold: 30
      periodSeconds: 5
```

### 추가 볼륨/환경변수

RSA 키 같은 파일을 마운트하거나 추가 환경변수를 주입할 때:

```yaml
gateway:
  extraVolumes:
    - name: rsa-keys
      secret:
        secretName: apex-rsa-keys
  extraVolumeMounts:
    - name: rsa-keys
      mountPath: /app/keys
      readOnly: true
  extraEnv:
    - name: LOG_LEVEL
      value: "debug"
```

### ServiceMonitor (Prometheus)

```yaml
auth-svc:
  serviceMonitor:
    enabled: true
    port: metrics
    path: /metrics
    interval: 15s
```

---

## 8. 새 서비스 추가

새 마이크로서비스를 추가할 때의 절차. 상세한 템플릿 사용법은 `charts/apex-common/README.md`를 참조.

### Step 1: sub-chart 디렉토리 생성

```bash
mkdir -p charts/my-new-svc/templates/tests
```

### Step 2: Chart.yaml 작성

```yaml
# charts/my-new-svc/Chart.yaml
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

### Step 3: values.yaml 작성

기존 서비스(예: `auth-svc/values.yaml`)를 복사한 뒤 수정:
- `image.repository` 변경
- `service.ports` 변경 (metrics 포트)
- `config.content`에 서비스 TOML 작성
- 필요 시 `extraVolumes`, `extraEnv` 추가
- `probes` 포트/경로 조정

### Step 4: 템플릿 파일 작성 (최소 3파일)

```yaml
# templates/deployment.yaml
{{- include "apex-common.deployment" . }}

# templates/service.yaml
{{- include "apex-common.service" . }}

# templates/configmap.yaml
{{- include "apex-common.configmap" . }}
```

필요하면 test-connection.yaml도 추가:

```yaml
# templates/tests/test-connection.yaml
apiVersion: v1
kind: Pod
metadata:
  name: "{{ include "apex-common.fullname" . }}-test"
  annotations:
    "helm.sh/hook": test
spec:
  containers:
    - name: test
      image: busybox:1.37
      command: ['sh', '-c', 'nc -z {{ include "apex-common.fullname" . }} <metrics-port> && echo SUCCESS']
  restartPolicy: Never
```

### Step 5: umbrella Chart.yaml에 의존성 추가

```yaml
# apex-pipeline/Chart.yaml
dependencies:
  # ... 기존 항목 ...
  - name: my-new-svc
    version: "0.1.0"
    condition: my-new-svc.enabled
```

### Step 6: umbrella values.yaml에 토글 추가

```yaml
# values.yaml
my-new-svc:
  enabled: true
```

### Step 7: 의존성 갱신 + 검증

```bash
helm dependency update .
helm lint .
helm template . --debug
```

---

## 9. 시크릿 관리

### expand_env 패턴

Apex Pipeline의 서비스들은 `apex::shared::expand_env()` 함수를 이미 구현하고 있다. 이 함수가 TOML 설정 파일의 `${VAR:-default}` 패턴을 런타임에 환경변수로 치환한다.

K8s에서의 시크릿 주입 흐름:

```
K8s Secret  -->  envFrom (환경변수)  -->  컨테이너 시작
ConfigMap   -->  volumeMount (TOML)  -->  expand_env가 ${VAR:-default} 치환
```

TOML에서의 사용 예:

```toml
[redis]
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"
```

- 환경변수 `REDIS_AUTH_PASSWORD`가 있으면 그 값을 사용
- 없으면 `dev_redis_pass`를 기본값으로 사용

### 로컬 환경

`secrets.data`에 평문으로 기본값을 지정한다. Helm이 이 값으로 K8s Secret을 자동 생성한다:

```yaml
# charts/gateway/values.yaml
secrets:
  data:
    REDIS_AUTH_PASSWORD: dev_redis_pass
    REDIS_RATELIMIT_PASSWORD: dev_redis_pass
```

### 프로덕션 환경

운영팀이 K8s Secret을 사전 생성하고, `existingSecret`으로 참조한다:

```yaml
# values-prod.yaml
gateway:
  secrets:
    existingSecret: "apex-gateway-secrets"
```

이 경우 Helm은 Secret을 생성하지 않고, 기존 Secret을 `envFrom`으로 마운트한다.

### RSA 키

| 환경 | 설정 | 동작 |
|------|------|------|
| 로컬 | `rsaKeys.create: true` | umbrella `templates/secrets.yaml`에서 테스트 키로 Secret 생성 |
| 프로덕션 | `rsaKeys.existingSecret: "apex-rsa-keys"` | 운영팀이 사전 생성한 Secret 참조 |

Gateway는 RSA 공개키만, Auth는 키쌍 전체를 마운트한다.

### 서비스별 필요 시크릿

| 서비스 | 환경변수 | 추가 볼륨 |
|--------|----------|-----------|
| Gateway | `REDIS_AUTH_PASSWORD`, `REDIS_RATELIMIT_PASSWORD`, `REDIS_PUBSUB_PASSWORD` | RSA 공개키 |
| Auth | `REDIS_AUTH_PASSWORD`, `PG_SERVICE_PASSWORD` | RSA 키쌍 |
| Chat | `REDIS_CHAT_PASSWORD`, `REDIS_PUBSUB_PASSWORD`, `PG_SERVICE_PASSWORD` | - |
| Log | (현재 없음) | - |

---

## 10. Ingress 설정

### 기본 구성

Ingress는 Gateway sub-chart에만 설정되어 있다. Auth/Chat/Log는 Kafka consumer로 inbound 앱 포트가 없으므로 Ingress가 불필요하다.

Ingress 컨트롤러: **kubernetes/ingress-nginx** (커뮤니티 표준 버전, NGINX Inc 상용 버전이 아님).

### WebSocket 특이사항

WebSocket 연결은 일반 HTTP와 달리 오래 유지되는 연결이다. 기본 nginx 타임아웃(60초)이면 WebSocket이 끊긴다. 이를 위해 다음 annotation이 설정되어 있다:

```yaml
annotations:
  nginx.ingress.kubernetes.io/proxy-read-timeout: "3600"    # 1시간
  nginx.ingress.kubernetes.io/proxy-send-timeout: "3600"
  nginx.ingress.kubernetes.io/proxy-http-version: "1.1"      # WS upgrade 자동 처리
  nginx.ingress.kubernetes.io/upstream-hash-by: "$remote_addr" # sticky session (선택)
```

`proxy-http-version: "1.1"` 설정 시 nginx가 `Connection: Upgrade` 헤더를 자동 처리하므로 별도 설정이 필요 없다.

### TLS

```
클라이언트 --[wss://]--> Ingress --[ws://]--> Gateway Pod
```

TLS termination은 Ingress에서 수행한다. Gateway 자체는 plain WebSocket(`ws://`)으로 수신하며 `[tls]` 설정이 없다.

| 환경 | TLS 인증서 |
|------|-----------|
| 로컬 | minikube 자체 서명 인증서 |
| 프로덕션 | cert-manager 자동 발급 (BACKLOG-197 예정) |

### minikube에서 Ingress 접근

```bash
# 1. Ingress addon 활성화 (local-setup.sh가 자동 수행)
minikube addons enable ingress

# 2. 별도 터미널에서 tunnel 실행 (LoadBalancer IP 할당)
minikube tunnel

# 3. /etc/hosts에 도메인 매핑 (Windows: C:\Windows\System32\drivers\etc\hosts)
# 127.0.0.1 apex.local

# 4. 접속
# wss://apex.local
```

`minikube tunnel`이 없으면 Ingress에 External IP가 할당되지 않아 외부 접근이 불가능하다.

---

## 11. 트러블슈팅

### Pod이 ImagePullBackOff 상태

```bash
kubectl describe pod <pod-name> -n apex-services
```

**원인**: 이미지를 Pull할 수 없음.

**해결**:
- 이미지 이름/태그 확인: `helm get values apex-services -n apex-services`
- private registry면 `imagePullSecrets` 설정 확인
- minikube에서 로컬 이미지 사용 시: `eval $(minikube docker-env)` 후 이미지 빌드

### Pod이 Pending 상태

```bash
kubectl describe pod <pod-name> -n <namespace>
kubectl get events -n <namespace> --sort-by='.lastTimestamp'
```

**원인**: 리소스 부족 또는 PVC 바인딩 실패.

**해결**:
- CPU/메모리 부족: `minikube start --memory=8192 --cpus=4`로 리소스 증가
- PVC Pending: `kubectl get pvc -n apex-infra`로 확인. minikube는 `standard` StorageClass가 기본 제공됨

### Pod이 CrashLoopBackOff 상태

```bash
kubectl logs <pod-name> -n <namespace>
kubectl logs <pod-name> -n <namespace> --previous    # 이전 크래시 로그
```

**원인**: 서비스 시작 실패 (설정 오류, 인프라 연결 실패 등).

**해결**:
- ConfigMap의 TOML 설정 확인: `kubectl get configmap -n apex-services -o yaml`
- 인프라가 먼저 준비되었는지 확인: `kubectl get pods -n apex-infra` (모두 Running이어야 함)
- startupProbe 시간 부족: `failureThreshold` 또는 `periodSeconds` 증가

### Ingress로 접속이 안 됨

```bash
kubectl get ingress -n apex-services
kubectl describe ingress <name> -n apex-services
kubectl get svc -n ingress-nginx
```

**확인 사항**:
1. `minikube tunnel`이 실행 중인가?
2. `/etc/hosts`에 `127.0.0.1 apex.local` 매핑이 있는가?
3. Ingress controller Pod이 Running 상태인가? (`kubectl get pods -n ingress-nginx`)
4. `ADDRESS` 필드에 IP가 할당되어 있는가?

### helm test 실패

```bash
helm test apex-services -n apex-services
kubectl logs <test-pod-name> -n apex-services
```

**원인**: 서비스 포트에 연결할 수 없음.

**해결**:
- 해당 서비스 Pod이 Running + Ready 상태인지 확인
- Service의 포트 매핑이 올바른지 확인: `kubectl get svc -n apex-services`

### Bitnami chart 설치 실패 (의존성)

```bash
helm dependency update .
```

**원인**: chart repository가 추가되지 않았거나 버전이 맞지 않음.

**해결**:
```bash
helm repo add bitnami https://charts.bitnami.com/bitnami
helm repo update
helm dependency update .
```

### Kafka가 시작되지 않음

Bitnami Kafka는 KRaft 모드로 동작하며 초기 시작에 시간이 걸린다.

```bash
kubectl logs <kafka-pod> -n apex-infra -f
kubectl get pods -n apex-infra -w
```

리소스가 부족하면 OOMKilled가 발생할 수 있다. minikube 메모리를 8GB로 늘려보자.

---

## 12. 명령어 레퍼런스

모든 명령은 `apex_infra/k8s/apex-pipeline/` 디렉토리에서 실행한다.

### helm dependency update

Bitnami chart 등 외부 의존성을 다운로드하고 `Chart.lock`을 갱신한다. sub-chart를 추가/변경한 후 반드시 실행해야 한다.

```bash
helm dependency update .
```

### helm install

```bash
# 인프라 release
helm install apex-infra . -n apex-infra --create-namespace \
  --set gateway.enabled=false \
  --set auth-svc.enabled=false \
  --set chat-svc.enabled=false \
  --set log-svc.enabled=false

# 서비스 release
helm install apex-services . -n apex-services --create-namespace \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false

# 프로덕션 (서비스만)
helm install apex-services . -n apex-services \
  -f values.yaml -f values-prod.yaml \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false
```

### helm upgrade

기존 release를 업그레이드한다. `--install` 플래그를 붙이면 없을 때 자동 설치한다.

```bash
helm upgrade --install apex-services . -n apex-services \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false \
  --wait --timeout 5m
```

### helm uninstall

```bash
helm uninstall apex-services -n apex-services
helm uninstall apex-infra -n apex-infra
```

순서 중요: 서비스를 먼저 삭제한 뒤 인프라를 삭제한다.

### helm test

각 서비스의 `test-connection.yaml`을 실행하여 포트 연결을 확인한다.

```bash
helm test apex-services -n apex-services
```

### helm template

실제 배포 없이 렌더링된 YAML을 확인한다. 디버깅에 유용하다.

```bash
# 전체 렌더링
helm template apex-services . \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false

# 특정 서비스만
helm template apex-services . -s charts/gateway/templates/deployment.yaml
```

### helm lint

차트 구문 오류를 검사한다.

```bash
helm lint .
helm lint . -f values-prod.yaml
```

### helm get

배포된 release의 상태를 확인한다.

```bash
helm get values apex-services -n apex-services        # 적용된 values
helm get manifest apex-services -n apex-services      # 배포된 매니페스트
helm list -n apex-services                             # release 목록
helm history apex-services -n apex-services            # 배포 이력
```
