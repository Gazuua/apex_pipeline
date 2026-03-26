# Loki + Vector 옵저빌리티 스택 구축

- **백로그**: BACKLOG-199
- **브랜치**: feature/log-svc-implementation

## 요약

C++ log-svc(KafkaSink 기반)를 Loki + Vector + Grafana 인프라 스택으로 대체.
성능 이득 없는 커스텀 로그 서비스 대신 검증된 오픈소스 옵저빌리티 도구 채택.

## 변경 사항

### 제거

- **KafkaSink** (`kafka_sink.hpp`, `kafka_sink.cpp`) — spdlog → Kafka 발행 sink. 서비스 코드 의존 0건
- **KafkaConfig.log_topic** 필드 — KafkaSink 전용 설정
- **KafkaSink 테스트** — 유닛 4케이스 + 통합 1케이스
- **log-svc C++ 스켈레톤** — `.gitkeep`만 있던 빈 디렉토리
- **log-svc Helm sub-chart** — C++ 바이너리 기반 Deployment/ConfigMap/probes 전체

### 추가

- **apex-observability Helm 차트** (`apex_infra/k8s/apex-observability/`)
  - Loki (monolithic, SingleBinary, filesystem PV, 7일 보존)
  - Vector (DaemonSet Agent, kubernetes_logs source, JSON 파싱 transform, Loki sink)
  - Grafana (Loki datasource 자동 프로비저닝)
  - 로컬 `values.yaml` + 프로덕션 `values-prod.yaml` (30일 보존, 리소스 강화)

### 인프라 변경

- umbrella Chart.yaml에서 log-svc dependency 제거
- values.yaml, values-prod.yaml에서 log-svc 섹션 제거
- local-setup.sh에서 log-svc 비활성화 플래그 제거
- 배포 순서: 2-release → 3-release (infra → observability → services)

## 데이터 플로우

```
서비스 Pod stdout (JSON) → Vector DaemonSet → Loki → Grafana (LogQL)
```

## 영향 범위

- `apex_shared/` — KafkaSink 제거 (kafka adapter 라이브러리, 테스트, CMakeLists)
- `apex_services/log-svc/` — 전체 삭제
- `apex_infra/k8s/` — log-svc 차트 삭제 + apex-observability 차트 신규
- 문서 전반 — KafkaSink 참조 갱신, 옵저빌리티 스택 추가
