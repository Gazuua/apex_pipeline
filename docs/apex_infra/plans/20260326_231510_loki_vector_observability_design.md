# Loki + Vector 기반 중앙 로그 시스템 설계

- **백로그**: BACKLOG-199
- **브랜치**: feature/log-svc-implementation
- **일자**: 2026-03-26

## 배경

BACKLOG-199는 원래 C++ ServiceBase 기반 log-svc 구현을 계획했으나, 아래 이유로 Loki + Vector 인프라 기반으로 전환:

1. **성능 이득 없음** — 자체 C++ 로그 서비스가 Loki 대비 유의미한 성능 이점을 제공하지 않음
2. **중복 개발** — 로그 저장/검색/시각화는 이미 성숙한 오픈소스 솔루션이 존재
3. **운영 부담** — 커스텀 로그 서비스는 저장 엔진, 쿼리 API, UI를 모두 자체 구현해야 함

기존 KafkaSink(spdlog → Kafka)도 함께 제거. 서비스 코드에서 사용하는 곳이 0건이며, stdout 기반 수집으로 전환하면 로그 경로에서 Kafka 의존성이 사라져 더 단순해짐.

## 아키텍처

```
┌─────────────────────────────────────────────────┐
│  K8s Cluster                                    │
│                                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐       │
│  │ Gateway  │ │ Auth-svc │ │ Chat-svc │  ...   │
│  │ (stdout) │ │ (stdout) │ │ (stdout) │        │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘       │
│       │             │            │              │
│       ▼             ▼            ▼              │
│  ┌──────────────────────────────────────┐      │
│  │  Vector DaemonSet (노드당 1개)        │      │
│  │  source: kubernetes_logs             │      │
│  │  transform: JSON 파싱 + 라벨 추출     │      │
│  │  sink: Loki push API                 │      │
│  └──────────────────┬───────────────────┘      │
│                     │                           │
│                     ▼                           │
│  ┌──────────────────────────────────────┐      │
│  │  Loki (monolithic mode)              │      │
│  │  label 인덱싱 + 로그 압축 저장        │      │
│  │  storage: PV (filesystem)            │      │
│  └──────────────────┬───────────────────┘      │
│                     │                           │
│                     ▼                           │
│  ┌──────────────────────────────────────┐      │
│  │  Grafana                             │      │
│  │  datasource: Loki (자동 프로비저닝)    │      │
│  │  LogQL 쿼리 + 대시보드 + 알림         │      │
│  └──────────────────────────────────────┘      │
└─────────────────────────────────────────────────┘
```

### 컴포넌트

| 컴포넌트 | K8s 리소스 | 역할 |
|----------|-----------|------|
| **Vector** | DaemonSet | 각 노드에서 Pod stdout 수집, JSON 파싱, Loki로 전송 |
| **Loki** | StatefulSet | 라벨 기반 인덱싱, 로그 압축 저장 (filesystem PV) |
| **Grafana** | Deployment | Loki datasource 자동 프로비저닝, LogQL 검색/시각화/알림 |

### 데이터 플로우

1. 서비스들이 spdlog를 통해 **stdout에 JSON 출력** (기존 포맷: `{"ts", "level", "logger", "msg", "file", "line"}`)
2. kubelet이 각 Pod의 stdout을 노드 파일시스템에 저장
3. **Vector DaemonSet**이 노드의 로그 파일을 tail하며 수집
4. Vector의 `remap` transform이 JSON 파싱 + K8s 메타데이터(namespace, pod, container) 추출
5. Vector가 **Loki push API**로 로그 전송 (라벨: namespace, pod, level, logger)
6. **Loki**가 라벨 인덱싱 + 로그 본문 압축 저장
7. **Grafana**에서 LogQL로 검색/시각화

## Helm 차트

### 차트 구조

```
apex_infra/k8s/apex-observability/
├── Chart.yaml
├── values.yaml
├── values-prod.yaml
└── templates/
    └── grafana-datasource.yaml
```

### Chart.yaml (dependencies)

```yaml
apiVersion: v2
name: apex-observability
description: Apex Observability Stack — Loki + Vector + Grafana
type: application
version: 0.1.0
appVersion: "0.6.5"

dependencies:
  - name: loki
    version: "~6.x"
    repository: https://grafana.github.io/helm-charts
    condition: loki.enabled
  - name: vector
    version: "~0.x"
    repository: https://helm.vector.dev
    condition: vector.enabled
  - name: grafana
    version: "~8.x"
    repository: https://grafana.github.io/helm-charts
    condition: grafana.enabled
```

### Vector 설정 (values.yaml)

```yaml
vector:
  enabled: true
  role: Agent
  customConfig:
    sources:
      kubernetes_logs:
        type: kubernetes_logs
    transforms:
      parse_json:
        type: remap
        inputs: ["kubernetes_logs"]
        source: |
          . = parse_json!(.message) ?? .
          .namespace = .kubernetes.pod_namespace
          .pod = .kubernetes.pod_name
          .container = .kubernetes.container_name
    sinks:
      loki:
        type: loki
        inputs: ["parse_json"]
        endpoint: "http://{{ .Release.Name }}-loki:3100"
        labels:
          namespace: "{{ `{{ namespace }}` }}"
          pod: "{{ `{{ pod }}` }}"
          level: "{{ `{{ level }}` }}"
          logger: "{{ `{{ logger }}` }}"
        encoding:
          codec: json
```

### Loki 설정 (values.yaml)

```yaml
loki:
  enabled: true
  deploymentMode: SingleBinary    # monolithic, 추후 분산 전환 가능
  loki:
    auth_enabled: false           # 단일 테넌트
    commonConfig:
      replication_factor: 1
    storage:
      type: filesystem
    limits_config:
      retention_period: 168h      # 7일
  singleBinary:
    replicas: 1
    persistence:
      enabled: true
      size: 10Gi
```

### Grafana 설정 (values.yaml)

```yaml
grafana:
  enabled: true
  persistence:
    enabled: true
    size: 1Gi
  datasources:
    datasources.yaml:
      apiVersion: 1
      datasources:
        - name: Loki
          type: loki
          url: "http://{{ .Release.Name }}-loki:3100"
          access: proxy
          isDefault: true
  service:
    type: ClusterIP
    port: 3000
```

### 프로덕션 오버라이드 (values-prod.yaml)

```yaml
loki:
  singleBinary:
    replicas: 1
    persistence:
      size: 50Gi
    resources:
      requests:
        cpu: 500m
        memory: 512Mi
      limits:
        cpu: "2"
        memory: 2Gi
  loki:
    limits_config:
      retention_period: 720h      # 30일

vector:
  resources:
    requests:
      cpu: 100m
      memory: 128Mi
    limits:
      cpu: 500m
      memory: 256Mi

grafana:
  persistence:
    size: 5Gi
  adminPassword: "${GRAFANA_ADMIN_PASSWORD}"
```

## 배포 순서

기존 2-release에서 3-release로 확장:

| 순서 | Release | 차트 | 내용 |
|------|---------|------|------|
| 1 | `apex-infra` | `apex-pipeline` | Kafka, Redis, PostgreSQL, pgbouncer |
| 2 | `apex-observability` | `apex-observability` (신규) | Loki, Vector, Grafana |
| 3 | `apex-services` | `apex-pipeline` | Gateway, Auth, Chat |

```bash
# 2. Observability release (신규)
helm upgrade --install apex-observability ./apex-observability \
  -n apex-observability --create-namespace \
  --wait --timeout 5m
```

## 제거 항목

### KafkaSink 제거

**파일 삭제** (4개):
- `apex_shared/lib/adapters/kafka/src/kafka_sink.cpp`
- `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_sink.hpp`
- `apex_shared/tests/unit/test_kafka_sink.cpp`
- `apex_shared/tests/integration/test_kafka_sink.cpp`

**파일 수정** (5개):
- `apex_shared/lib/adapters/kafka/CMakeLists.txt` — `src/kafka_sink.cpp` 제거
- `apex_shared/tests/unit/CMakeLists.txt` — `test_kafka_sink` 제거
- `apex_shared/tests/integration/CMakeLists.txt` — `test_kafka_sink` 제거
- `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp` — `log_topic` 필드 제거
- `apex_shared/tests/unit/test_kafka_producer.cpp` — `log_topic` assertion 제거

### log-svc C++ 스켈레톤 제거

- `apex_services/log-svc/include/apex/log_svc/.gitkeep`
- `apex_services/log-svc/src/.gitkeep`
- `apex_services/log-svc/tests/.gitkeep`

### log-svc Helm sub-chart 제거

- `apex_infra/k8s/apex-pipeline/charts/log-svc/` 전체 삭제
- `apex_infra/k8s/apex-pipeline/Chart.yaml` — log-svc dependency 제거
- `apex_infra/k8s/apex-pipeline/values.yaml` — log-svc 섹션 제거
- `apex_infra/k8s/apex-pipeline/values-prod.yaml` — log-svc 섹션 제거 (있다면)

## 문서 갱신

- `README.md` — KafkaSink 제거, 옵저빌리티 스택 추가
- `docs/Apex_Pipeline.md` — 아키텍처, 로드맵, 변경 이력
- `CLAUDE.md` — 로드맵, 모노레포 구조에 apex-observability 추가
- `apex_shared/README.md` — KafkaSink 참조 제거
- `docs/apex_infra/cicd_guide.md` — 3-release 배포 순서 갱신
- progress 문서 작성
