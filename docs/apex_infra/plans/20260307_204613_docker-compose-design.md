# docker-compose 로컬 개발 환경 설계

**작성일**: 2026-03-07
**상태**: 승인됨

---

## 목적

apex_core 개발(외부 어댑터 연동 테스트)과 서비스 레이어(v0.4.0+) 개발을 위한 로컬 인프라 환경 구축.

## 결정 사항

| 항목 | 선택 | 근거 |
|------|------|------|
| Docker 환경 | Docker Desktop (Windows, WSL2) | 이미 설치됨, 같은 터미널에서 사용 가능 |
| 구성 방식 | 단일 docker-compose.yml + profiles | 서비스 수 적음, YAGNI |
| Kafka | `apache/kafka:latest`, KRaft | 엔터프라이즈 실무 표준 |
| Redis | `redis:7-alpine` | 경량, 로컬 개발에 충분 |
| PostgreSQL | `postgres:16-alpine` | 설계 문서 기준 16+ |
| Prometheus | `prom/prometheus:latest` | observability 프로파일 |
| Grafana | `grafana/grafana:latest` | Prometheus 자동 프로비저닝 |
| 포트 | 전부 기본 포트 | 충돌 없음 확인 |
| 데이터 영속성 | Named Volume | 재시작 시 데이터 유지, `down -v`로 초기화 |
| 네트워크 | `apex-net` 브릿지 | 컨테이너 간 서비스명으로 통신 |

## 프로파일

| 프로파일 | 서비스 | 명령어 |
|----------|--------|--------|
| *(기본)* | Kafka, Redis, PostgreSQL | `docker compose up -d` |
| observability | + Prometheus, Grafana | `docker compose --profile observability up -d` |
| full | + 서비스 컨테이너 (향후) | 향후 추가 |

## 서비스 상세

### Kafka (KRaft 단일 브로커)

- controller + broker 겸용
- KAFKA_NODE_ID=1
- 리스너: PLAINTEXT://0.0.0.0:9092 (브로커) + CONTROLLER://0.0.0.0:9093
- ADVERTISED_LISTENERS: PLAINTEXT://localhost:9092
- 클러스터 ID 고정 (로컬 개발용)

### Redis

- 비밀번호 없음 (로컬 개발)
- maxmemory 256mb, allkeys-lru 정책

### PostgreSQL

- DB: apex, User: apex, Password: apex
- init.sql로 서비스별 스키마 3개 자동 생성 (auth_schema, chat_schema, match_schema)
- 테이블 DDL은 포함하지 않음 (서비스 구현 시 마이그레이션으로 추가)

### Prometheus (observability)

- scrape_interval: 15s
- scrape 타겟 빈 상태 (서비스 추가 시 갱신)

### Grafana (observability)

- admin/admin 로그인
- Prometheus 데이터소스 자동 프로비저닝

## 산출물

```
apex_infra/
├── docker-compose.yml
├── postgres/init.sql
├── prometheus/prometheus.yml
├── grafana/provisioning/datasources/prometheus.yml
└── README.md
```

## 사용법

```bash
# minimal (기본)
docker compose up -d

# observability 추가
docker compose --profile observability up -d

# 종료 (데이터 유지)
docker compose down

# 종료 + 데이터 초기화
docker compose down -v
```
