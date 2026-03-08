# apex_infra

인프라 및 배포 설정.

## 로컬 개발 환경 (docker-compose)

### 필수 조건

- Docker Desktop (Windows, WSL2 백엔드)

### 서비스 구성

| 서비스 | 이미지 | 포트 | 프로파일 |
|--------|--------|------|----------|
| Kafka (KRaft) | apache/kafka | 9092 | 기본 |
| Redis | redis:7-alpine | 6379 | 기본 |
| PostgreSQL | postgres:16-alpine | 5432 | 기본 |
| Prometheus | prom/prometheus | 9090 | observability |
| Grafana | grafana/grafana | 3000 | observability |

### 사용법

```bash
# minimal — Kafka, Redis, PostgreSQL
docker compose up -d

# observability 추가 — + Prometheus, Grafana
docker compose --profile observability up -d

# 상태 확인
docker compose ps

# 종료 (데이터 유지)
docker compose down

# 종료 + 데이터 초기화
docker compose down -v
```

### PostgreSQL 접속

```bash
docker exec -it apex-postgres psql -U apex -d apex
```

### Redis 접속

```bash
docker exec -it apex-redis redis-cli
```

## K8s

- `k8s/` — Helm charts (서비스별, Phase 9에서 추가)
