# docker-compose 로컬 개발 환경 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Kafka(KRaft), Redis, PostgreSQL + Prometheus, Grafana를 로컬에서 한 명령어로 띄우는 docker-compose 환경 구축

**Architecture:** 단일 docker-compose.yml에 profiles로 minimal(기본)/observability를 분리. PostgreSQL은 init.sql로 스키마 초기화, Grafana는 Prometheus 데이터소스를 자동 프로비저닝.

**Tech Stack:** Docker Compose, apache/kafka (KRaft), redis:7-alpine, postgres:16-alpine, prom/prometheus, grafana/grafana

**설계 문서:** `docs/apex_infra/plans/20260307_204613_docker-compose-design.md`

**충돌 방지:** apex_core/ 하위 파일 수정 금지 (별도 에이전트 담당)

---

### Task 1: PostgreSQL 초기화 스크립트

**Files:**
- Create: `apex_infra/postgres/init.sql`

**Step 1: init.sql 작성**

```sql
-- Apex Pipeline: 서비스별 스키마 초기화
-- docker-entrypoint-initdb.d에 마운트되어 최초 기동 시 자동 실행

CREATE SCHEMA IF NOT EXISTS auth_schema;
CREATE SCHEMA IF NOT EXISTS chat_schema;
CREATE SCHEMA IF NOT EXISTS match_schema;
```

**Step 2: 커밋 (아직 안 함 — Task 6에서 일괄 커밋)**

---

### Task 2: Prometheus 설정 파일

**Files:**
- Create: `apex_infra/prometheus/prometheus.yml`

**Step 1: prometheus.yml 작성**

```yaml
global:
  scrape_interval: 15s
  evaluation_interval: 15s

scrape_configs:
  - job_name: "prometheus"
    static_configs:
      - targets: ["localhost:9090"]
```

**Step 2: 검증 — YAML 문법 확인**

Run: `python -c "import yaml; yaml.safe_load(open('apex_infra/prometheus/prometheus.yml'))"`
Expected: 에러 없이 종료

---

### Task 3: Grafana 데이터소스 프로비저닝

**Files:**
- Create: `apex_infra/grafana/provisioning/datasources/prometheus.yml`

**Step 1: 프로비저닝 파일 작성**

```yaml
apiVersion: 1

datasources:
  - name: Prometheus
    type: prometheus
    access: proxy
    url: http://prometheus:9090
    isDefault: true
    editable: false
```

---

### Task 4: docker-compose.yml 작성

**Files:**
- Create: `apex_infra/docker-compose.yml`

**Step 1: docker-compose.yml 작성**

전체 내용은 아래와 같다. 핵심 포인트:
- minimal 서비스(kafka, redis, postgres)는 profiles 없이 항상 기동
- observability 서비스(prometheus, grafana)는 `profiles: [observability]`로 분리
- 모든 서비스가 `apex-net` 네트워크에 연결
- Named Volume 5개로 데이터 영속화

```yaml
services:
  kafka:
    image: apache/kafka:latest
    container_name: apex-kafka
    ports:
      - "9092:9092"
    environment:
      KAFKA_NODE_ID: 1
      KAFKA_PROCESS_ROLES: broker,controller
      KAFKA_LISTENERS: PLAINTEXT://0.0.0.0:9092,CONTROLLER://0.0.0.0:9093
      KAFKA_ADVERTISED_LISTENERS: PLAINTEXT://localhost:9092
      KAFKA_CONTROLLER_LISTENER_NAMES: CONTROLLER
      KAFKA_LISTENER_SECURITY_PROTOCOL_MAP: CONTROLLER:PLAINTEXT,PLAINTEXT:PLAINTEXT
      KAFKA_CONTROLLER_QUORUM_VOTERS: 1@kafka:9093
      KAFKA_OFFSETS_TOPIC_REPLICATION_FACTOR: 1
      KAFKA_TRANSACTION_STATE_LOG_REPLICATION_FACTOR: 1
      KAFKA_TRANSACTION_STATE_LOG_MIN_ISR: 1
      KAFKA_LOG_DIRS: /var/lib/kafka/data
      CLUSTER_ID: apex-local-cluster-001
    volumes:
      - kafka-data:/var/lib/kafka/data
    networks:
      - apex-net
    healthcheck:
      test: /opt/kafka/bin/kafka-broker-api-versions.sh --bootstrap-server localhost:9092 > /dev/null 2>&1
      interval: 10s
      timeout: 5s
      retries: 5

  redis:
    image: redis:7-alpine
    container_name: apex-redis
    ports:
      - "6379:6379"
    command: redis-server --maxmemory 256mb --maxmemory-policy allkeys-lru
    volumes:
      - redis-data:/data
    networks:
      - apex-net
    healthcheck:
      test: redis-cli ping | grep -q PONG
      interval: 10s
      timeout: 3s
      retries: 3

  postgres:
    image: postgres:16-alpine
    container_name: apex-postgres
    ports:
      - "5432:5432"
    environment:
      POSTGRES_DB: apex
      POSTGRES_USER: apex
      POSTGRES_PASSWORD: apex
    volumes:
      - postgres-data:/var/lib/postgresql/data
      - ./postgres/init.sql:/docker-entrypoint-initdb.d/init.sql:ro
    networks:
      - apex-net
    healthcheck:
      test: pg_isready -U apex -d apex
      interval: 10s
      timeout: 3s
      retries: 3

  prometheus:
    image: prom/prometheus:latest
    container_name: apex-prometheus
    profiles:
      - observability
    ports:
      - "9090:9090"
    volumes:
      - prometheus-data:/prometheus
      - ./prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro
    networks:
      - apex-net

  grafana:
    image: grafana/grafana:latest
    container_name: apex-grafana
    profiles:
      - observability
    ports:
      - "3000:3000"
    environment:
      GF_SECURITY_ADMIN_USER: admin
      GF_SECURITY_ADMIN_PASSWORD: admin
    volumes:
      - grafana-data:/var/lib/grafana
      - ./grafana/provisioning:/etc/grafana/provisioning:ro
    networks:
      - apex-net
    depends_on:
      - prometheus

volumes:
  kafka-data:
  redis-data:
  postgres-data:
  prometheus-data:
  grafana-data:

networks:
  apex-net:
    driver: bridge
```

**Step 2: 문법 검증**

Run: `docker compose -f apex_infra/docker-compose.yml config --quiet`
Expected: 에러 없이 종료 (YAML + Compose 문법 유효)

---

### Task 5: minimal 프로파일 동작 검증

**Step 1: 컨테이너 기동**

Run: `docker compose -f apex_infra/docker-compose.yml up -d`
Expected: kafka, redis, postgres 3개 컨테이너 기동

**Step 2: 헬스체크 대기 (최대 60초)**

Run: `docker compose -f apex_infra/docker-compose.yml ps`
Expected: 3개 서비스 모두 `Up` 또는 `healthy` 상태

**Step 3: 각 서비스 접속 확인**

```bash
# Redis
docker exec apex-redis redis-cli ping
# Expected: PONG

# PostgreSQL — 스키마 확인
docker exec apex-postgres psql -U apex -d apex -c "\dn"
# Expected: auth_schema, chat_schema, match_schema 3개 표시

# Kafka — 브로커 상태
docker exec apex-kafka /opt/kafka/bin/kafka-broker-api-versions.sh --bootstrap-server localhost:9092 | head -1
# Expected: 브로커 API 버전 정보 출력
```

**Step 4: 컨테이너 종료**

Run: `docker compose -f apex_infra/docker-compose.yml down`

---

### Task 6: observability 프로파일 동작 검증

**Step 1: 전체 기동**

Run: `docker compose -f apex_infra/docker-compose.yml --profile observability up -d`
Expected: 5개 컨테이너 기동

**Step 2: Prometheus 확인**

Run: `curl -s http://localhost:9090/-/healthy`
Expected: `Prometheus Server is Healthy.`

**Step 3: Grafana 확인**

Run: `curl -s -o /dev/null -w "%{http_code}" http://localhost:3000/api/health`
Expected: `200`

**Step 4: 종료 + 데이터 초기화**

Run: `docker compose -f apex_infra/docker-compose.yml --profile observability down -v`

---

### Task 7: README.md 갱신

**Files:**
- Modify: `apex_infra/README.md`

**Step 1: README 갱신**

기존 내용을 대체하여 사용법 포함:

```markdown
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

​```bash
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
​```

### PostgreSQL 접속

​```bash
docker exec -it apex-postgres psql -U apex -d apex
​```

### Redis 접속

​```bash
docker exec -it apex-redis redis-cli
​```

## K8s

- `k8s/` — Helm charts (서비스별, 향후 추가)
```

---

### Task 8: 커밋

**Step 1: 전체 변경 사항 스테이징 및 커밋**

```bash
git add apex_infra/docker-compose.yml \
        apex_infra/postgres/init.sql \
        apex_infra/prometheus/prometheus.yml \
        apex_infra/grafana/provisioning/datasources/prometheus.yml \
        apex_infra/README.md
git commit -m "infra: docker-compose 로컬 개발 환경 구축 (Kafka/Redis/PostgreSQL/Prometheus/Grafana)"
```

**Step 2: 검증**

Run: `git log --oneline -1`
Expected: 위 커밋 메시지 확인
