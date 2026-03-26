# Loki + Vector 옵저빌리티 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** KafkaSink + log-svc C++ 제거, Loki + Vector + Grafana 인프라 Helm 차트 구축

**Architecture:** 기존 C++ log-svc를 Loki + Vector + Grafana 인프라 스택으로 대체. KafkaSink (spdlog → Kafka) 완전 제거, stdout 기반 수집으로 전환. 별도 `apex-observability` umbrella chart로 분리.

**Tech Stack:** Helm, Grafana Loki, Vector (Datadog), Grafana

---

## File Structure

### 삭제 대상

| 파일 | 이유 |
|------|------|
| `apex_shared/lib/adapters/kafka/src/kafka_sink.cpp` | KafkaSink 구현 |
| `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_sink.hpp` | KafkaSink 헤더 |
| `apex_shared/tests/unit/test_kafka_sink.cpp` | KafkaSink 유닛 테스트 |
| `apex_shared/tests/integration/test_kafka_sink.cpp` | KafkaSink 통합 테스트 |
| `apex_services/log-svc/` | C++ 빈 스켈레톤 전체 |
| `apex_infra/k8s/apex-pipeline/charts/log-svc/` | C++ 기반 Helm sub-chart 전체 |

### 수정 대상

| 파일 | 변경 내용 |
|------|-----------|
| `apex_shared/lib/adapters/kafka/CMakeLists.txt:12` | `src/kafka_sink.cpp` 제거 |
| `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp:58-59` | `log_topic` 필드 제거 |
| `apex_shared/tests/unit/CMakeLists.txt:83` | `test_kafka_sink` 제거 |
| `apex_shared/tests/integration/CMakeLists.txt:36` | `test_kafka_sink` 제거 |
| `apex_shared/tests/unit/test_kafka_producer.cpp:17` | `log_topic` assertion 제거 |
| `apex_infra/k8s/apex-pipeline/Chart.yaml:20-22` | log-svc dependency 제거 |
| `apex_infra/k8s/apex-pipeline/values.yaml:72-75` | log-svc 섹션 제거 |
| `apex_infra/k8s/apex-pipeline/values-prod.yaml:68-69` | log-svc 섹션 제거 |
| `apex_infra/k8s/apex-pipeline/scripts/local-setup.sh:42` | `--set log-svc.enabled=false` 제거 |
| `apex_shared/README.md:13` | KafkaSink 참조 제거 |
| `README.md:535` | KafkaSink 참조 제거 |

### 생성 대상

| 파일 | 역할 |
|------|------|
| `apex_infra/k8s/apex-observability/Chart.yaml` | Loki + Vector + Grafana dependency 선언 |
| `apex_infra/k8s/apex-observability/values.yaml` | 로컬 기본값 (monolithic Loki, Vector Agent, Grafana) |
| `apex_infra/k8s/apex-observability/values-prod.yaml` | 프로덕션 오버라이드 (리소스, 보존 기간, 볼륨) |

---

### Task 1: KafkaSink C++ 코드 제거

**Files:**
- Delete: `apex_shared/lib/adapters/kafka/src/kafka_sink.cpp`
- Delete: `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_sink.hpp`
- Delete: `apex_shared/tests/unit/test_kafka_sink.cpp`
- Delete: `apex_shared/tests/integration/test_kafka_sink.cpp`
- Modify: `apex_shared/lib/adapters/kafka/CMakeLists.txt`
- Modify: `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp`
- Modify: `apex_shared/tests/unit/CMakeLists.txt`
- Modify: `apex_shared/tests/integration/CMakeLists.txt`
- Modify: `apex_shared/tests/unit/test_kafka_producer.cpp`

- [ ] **Step 1: KafkaSink 소스 파일 4개 삭제**

```bash
rm apex_shared/lib/adapters/kafka/src/kafka_sink.cpp
rm apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_sink.hpp
rm apex_shared/tests/unit/test_kafka_sink.cpp
rm apex_shared/tests/integration/test_kafka_sink.cpp
```

- [ ] **Step 2: kafka CMakeLists에서 kafka_sink.cpp 제거**

`apex_shared/lib/adapters/kafka/CMakeLists.txt` — line 12 `src/kafka_sink.cpp` 삭제:

```cmake
add_library(apex_shared_adapters_kafka STATIC
    src/kafka_config.cpp
    src/kafka_producer.cpp
    src/kafka_consumer.cpp
    src/kafka_adapter.cpp
    src/consumer_payload_pool.cpp
)
```

- [ ] **Step 3: KafkaConfig에서 log_topic 필드 제거**

`apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp` — lines 58-59 삭제:

변경 전:
```cpp
    size_t payload_pool_max_count = 4096;   ///< 풀 최대 크기 (0=무제한)

    // KafkaSink
    std::string log_topic = "apex-logs"; ///< log-dedicated topic

    // Common
```

변경 후:
```cpp
    size_t payload_pool_max_count = 4096;   ///< 풀 최대 크기 (0=무제한)

    // Common
```

- [ ] **Step 4: 유닛 테스트 CMakeLists에서 test_kafka_sink 제거**

`apex_shared/tests/unit/CMakeLists.txt` — line 83 삭제:

변경 전:
```cmake
apex_shared_add_kafka_test(test_kafka_producer test_kafka_producer.cpp)
apex_shared_add_kafka_test(test_kafka_consumer test_kafka_consumer.cpp)
apex_shared_add_kafka_test(test_kafka_adapter test_kafka_adapter.cpp)
apex_shared_add_kafka_test(test_kafka_sink test_kafka_sink.cpp)
apex_shared_add_kafka_test(test_consumer_payload_pool test_consumer_payload_pool.cpp)
```

변경 후:
```cmake
apex_shared_add_kafka_test(test_kafka_producer test_kafka_producer.cpp)
apex_shared_add_kafka_test(test_kafka_consumer test_kafka_consumer.cpp)
apex_shared_add_kafka_test(test_kafka_adapter test_kafka_adapter.cpp)
apex_shared_add_kafka_test(test_consumer_payload_pool test_consumer_payload_pool.cpp)
```

- [ ] **Step 5: 통합 테스트 CMakeLists에서 test_kafka_sink 제거**

`apex_shared/tests/integration/CMakeLists.txt` — line 36 삭제:

변경 전:
```cmake
apex_shared_add_integration_test(test_kafka_roundtrip test_kafka_roundtrip.cpp)
apex_shared_add_integration_test(test_redis_roundtrip test_redis_roundtrip.cpp)
apex_shared_add_integration_test(test_pg_roundtrip test_pg_roundtrip.cpp)
apex_shared_add_integration_test(test_kafka_sink test_kafka_sink.cpp)
```

변경 후:
```cmake
apex_shared_add_integration_test(test_kafka_roundtrip test_kafka_roundtrip.cpp)
apex_shared_add_integration_test(test_redis_roundtrip test_redis_roundtrip.cpp)
apex_shared_add_integration_test(test_pg_roundtrip test_pg_roundtrip.cpp)
```

- [ ] **Step 6: test_kafka_producer에서 log_topic assertion 제거**

`apex_shared/tests/unit/test_kafka_producer.cpp` — line 17 삭제:

변경 전:
```cpp
    EXPECT_EQ(config.brokers, "localhost:9092");
    EXPECT_EQ(config.consumer_group, "apex-group");
    EXPECT_EQ(config.log_topic, "apex-logs");
    EXPECT_EQ(config.compression_type, "lz4");
```

변경 후:
```cpp
    EXPECT_EQ(config.brokers, "localhost:9092");
    EXPECT_EQ(config.consumer_group, "apex-group");
    EXPECT_EQ(config.compression_type, "lz4");
```

- [ ] **Step 7: 커밋**

```bash
git add -A apex_shared/lib/adapters/kafka/ apex_shared/tests/
git commit -m "refactor(shared): KafkaSink 제거 — Loki+Vector로 대체 (BACKLOG-199)"
```

---

### Task 2: log-svc C++ 스켈레톤 + Helm sub-chart 제거

**Files:**
- Delete: `apex_services/log-svc/` (전체)
- Delete: `apex_infra/k8s/apex-pipeline/charts/log-svc/` (전체)
- Modify: `apex_infra/k8s/apex-pipeline/Chart.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/values-prod.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/scripts/local-setup.sh`

- [ ] **Step 1: log-svc C++ 스켈레톤 삭제**

```bash
rm -rf apex_services/log-svc/
```

`apex_services/CMakeLists.txt`는 log-svc를 참조하지 않으므로 변경 불필요.

- [ ] **Step 2: log-svc Helm sub-chart 삭제**

```bash
rm -rf apex_infra/k8s/apex-pipeline/charts/log-svc/
```

- [ ] **Step 3: umbrella Chart.yaml에서 log-svc dependency 제거**

`apex_infra/k8s/apex-pipeline/Chart.yaml` — lines 20-22 삭제:

변경 전:
```yaml
  - name: chat-svc
    version: "0.1.0"
    condition: chat-svc.enabled
  - name: log-svc
    version: "0.1.0"
    condition: log-svc.enabled

  # --- 인프라 (Bitnami, 로컬 전용) ---
```

변경 후:
```yaml
  - name: chat-svc
    version: "0.1.0"
    condition: chat-svc.enabled

  # --- 인프라 (Bitnami, 로컬 전용) ---
```

- [ ] **Step 4: umbrella values.yaml에서 log-svc 섹션 제거**

`apex_infra/k8s/apex-pipeline/values.yaml` — log-svc 블록 삭제 (lines 72-75):

변경 전:
```yaml
  rollouts:
    enabled: false

log-svc:
  enabled: false  # 바이너리 미구현 (BACKLOG-199)
  rollouts:
    enabled: false
```

변경 후 (chat-svc rollouts로 바로 이어짐):
```yaml
  rollouts:
    enabled: false
```

- [ ] **Step 5: values-prod.yaml에서 log-svc 섹션 제거**

`apex_infra/k8s/apex-pipeline/values-prod.yaml` — lines 68-69 삭제:

변경 전:
```yaml
  secrets:
    existingSecret: "apex-chat-secrets"

log-svc:
  enabled: false  # 구현 완료 시 true로 전환

# === 인프라 (프로덕션: 전부 비활성 — 관리형 서비스 사용) ===
```

변경 후:
```yaml
  secrets:
    existingSecret: "apex-chat-secrets"

# === 인프라 (프로덕션: 전부 비활성 — 관리형 서비스 사용) ===
```

- [ ] **Step 6: local-setup.sh에서 log-svc 비활성화 줄 제거**

`apex_infra/k8s/apex-pipeline/scripts/local-setup.sh` — line 42 삭제:

변경 전:
```bash
    --set auth-svc.enabled=false \
    --set chat-svc.enabled=false \
    --set log-svc.enabled=false \
    --wait --timeout 7m
```

변경 후:
```bash
    --set auth-svc.enabled=false \
    --set chat-svc.enabled=false \
    --wait --timeout 7m
```

- [ ] **Step 7: 커밋**

```bash
git add -A apex_services/log-svc/ apex_infra/k8s/apex-pipeline/
git commit -m "refactor(infra): log-svc C++ 스켈레톤 + Helm sub-chart 제거 (BACKLOG-199)"
```

---

### Task 3: apex-observability Helm 차트 생성

**Files:**
- Create: `apex_infra/k8s/apex-observability/Chart.yaml`
- Create: `apex_infra/k8s/apex-observability/values.yaml`
- Create: `apex_infra/k8s/apex-observability/values-prod.yaml`

- [ ] **Step 1: Chart.yaml 생성**

`apex_infra/k8s/apex-observability/Chart.yaml`:

```yaml
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
apiVersion: v2
name: apex-observability
description: Apex Observability Stack — Loki + Vector + Grafana (BACKLOG-199)
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

- [ ] **Step 2: values.yaml 생성**

`apex_infra/k8s/apex-observability/values.yaml`:

```yaml
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#
# Apex Observability Stack — 로컬 환경 기본값
# Loki (monolithic) + Vector (DaemonSet) + Grafana
# 프로덕션 오버라이드: values-prod.yaml

# ---------------------------------------------------------------------------
# Loki — 로그 저장/인덱싱
# ---------------------------------------------------------------------------
loki:
  enabled: true
  deploymentMode: SingleBinary
  loki:
    auth_enabled: false
    commonConfig:
      replication_factor: 1
    storage:
      type: filesystem
    limits_config:
      retention_period: 168h    # 7일
  singleBinary:
    replicas: 1
    persistence:
      enabled: true
      size: 10Gi
  # 로컬 환경: 최소 리소스
  resources:
    requests:
      cpu: 100m
      memory: 256Mi
    limits:
      cpu: 500m
      memory: 512Mi
  # Monitoring (자체 메트릭)
  monitoring:
    selfMonitoring:
      enabled: false
    lokiCanary:
      enabled: false
  # 로컬: gateway 비활성 (monolithic이면 불필요)
  gateway:
    enabled: false
  # 로컬: chunksCache/resultsCache 비활성
  chunksCache:
    enabled: false
  resultsCache:
    enabled: false

# ---------------------------------------------------------------------------
# Vector — 로그 수집 (DaemonSet)
# ---------------------------------------------------------------------------
vector:
  enabled: true
  role: Agent
  customConfig:
    data_dir: /vector-data-dir
    sources:
      kubernetes_logs:
        type: kubernetes_logs
    transforms:
      parse_structured:
        type: remap
        inputs:
          - kubernetes_logs
        source: |
          # spdlog JSON 형식 파싱 (ts, level, logger, msg, file, line)
          parsed, err = parse_json(.message)
          if err == null {
            .ts = parsed.ts
            .level = parsed.level
            .logger = parsed.logger
            .msg = parsed.msg
            .source_file = parsed.file
            .source_line = parsed.line
          }
    sinks:
      loki:
        type: loki
        inputs:
          - parse_structured
        endpoint: http://apex-observability-loki:3100
        labels:
          namespace: '{{ "{{ kubernetes.pod_namespace }}" }}'
          pod: '{{ "{{ kubernetes.pod_name }}" }}'
          container: '{{ "{{ kubernetes.container_name }}" }}'
          level: '{{ "{{ level }}" }}'
          logger: '{{ "{{ logger }}" }}'
        encoding:
          codec: json
  # 로컬 환경: 최소 리소스
  resources:
    requests:
      cpu: 50m
      memory: 64Mi
    limits:
      cpu: 200m
      memory: 128Mi

# ---------------------------------------------------------------------------
# Grafana — 시각화/대시보드
# ---------------------------------------------------------------------------
grafana:
  enabled: true
  adminUser: admin
  adminPassword: admin       # 로컬 전용 — 프로덕션은 existingSecret 사용
  persistence:
    enabled: true
    size: 1Gi
  datasources:
    datasources.yaml:
      apiVersion: 1
      datasources:
        - name: Loki
          type: loki
          url: http://apex-observability-loki:3100
          access: proxy
          isDefault: true
  service:
    type: ClusterIP
    port: 3000
  # 로컬 환경: 최소 리소스
  resources:
    requests:
      cpu: 50m
      memory: 64Mi
    limits:
      cpu: 200m
      memory: 256Mi
```

- [ ] **Step 3: values-prod.yaml 생성**

`apex_infra/k8s/apex-observability/values-prod.yaml`:

```yaml
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
# Apex Observability — 프로덕션 오버라이드
# 사용: helm install ... -f values.yaml -f values-prod.yaml

# ---------------------------------------------------------------------------
# Loki
# ---------------------------------------------------------------------------
loki:
  singleBinary:
    replicas: 1
    persistence:
      size: 50Gi
  loki:
    limits_config:
      retention_period: 720h    # 30일
  resources:
    requests:
      cpu: 500m
      memory: 512Mi
    limits:
      cpu: "2"
      memory: 2Gi

# ---------------------------------------------------------------------------
# Vector
# ---------------------------------------------------------------------------
vector:
  resources:
    requests:
      cpu: 100m
      memory: 128Mi
    limits:
      cpu: 500m
      memory: 256Mi

# ---------------------------------------------------------------------------
# Grafana
# ---------------------------------------------------------------------------
grafana:
  adminPassword: "${GRAFANA_ADMIN_PASSWORD}"    # 환경 변수 또는 existingSecret으로 교체
  persistence:
    size: 5Gi
  resources:
    requests:
      cpu: 100m
      memory: 128Mi
    limits:
      cpu: 500m
      memory: 512Mi
```

- [ ] **Step 4: 커밋**

```bash
git add apex_infra/k8s/apex-observability/
git commit -m "feat(infra): apex-observability Helm 차트 — Loki+Vector+Grafana (BACKLOG-199)"
```

---

### Task 4: 빌드 검증

- [ ] **Step 1: clang-format** (코드 변경 대상에 .cpp/.hpp 삭제만이므로 신규 포맷팅 불필요하지만, 변경된 파일에 대해 확인)

```bash
# kafka_config.hpp만 변경됨 — 포맷 확인
clang-format -i apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp
```

- [ ] **Step 2: 빌드 + 테스트**

```bash
"<project_root>/apex_tools/apex-agent/run-hook" queue build debug
```

빌드 성공 기준: KafkaSink 관련 심볼 없이 전체 빌드+테스트 통과.

---

### Task 5: 문서 갱신

**Files:**
- Modify: `README.md`
- Modify: `apex_shared/README.md`
- Modify: `docs/Apex_Pipeline.md` (변경 이력 + 아키텍처)
- Modify: `CLAUDE.md` (로드맵)
- Modify: `docs/apex_infra/cicd_guide.md` (3-release 배포)
- Create: `docs/apex_infra/progress/YYYYMMDD_HHMMSS_loki_vector_observability.md`

- [ ] **Step 1: README.md — KafkaSink 참조 갱신**

Line 535 변경:

변경 전:
```
  - Kafka 어댑터: librdkafka Producer/Consumer + Asio 통합 + KafkaSink (spdlog → Kafka)
```

변경 후:
```
  - Kafka 어댑터: librdkafka Producer/Consumer + Asio 통합
```

- [ ] **Step 2: apex_shared/README.md — KafkaSink 참조 제거**

Line 13 변경:

변경 전:
```
    - `kafka/` — KafkaProducer, KafkaConsumer, KafkaAdapter, KafkaSink
```

변경 후:
```
    - `kafka/` — KafkaProducer, KafkaConsumer, KafkaAdapter
```

- [ ] **Step 3: docs/Apex_Pipeline.md 갱신** — 아키텍처 섹션에 옵저빌리티 스택 추가, 변경 이력에 KafkaSink 제거 + Loki/Vector 추가 기록

- [ ] **Step 4: CLAUDE.md 로드맵 갱신** — 현재 버전 설명에 "KafkaSink 제거, Loki+Vector+Grafana 옵저빌리티" 추가, 모노레포 구조에 apex-observability 언급

- [ ] **Step 5: cicd_guide.md 갱신** — 배포 순서를 2-release에서 3-release로 갱신 (infra → observability → services)

- [ ] **Step 6: progress 문서 작성**

- [ ] **Step 7: 커밋**

```bash
git add README.md apex_shared/README.md docs/ CLAUDE.md
git commit -m "docs(infra): Loki+Vector 옵저빌리티 문서 갱신 (BACKLOG-199)"
```
