# E2E 테스트 — 실행 가이드 & 트러블슈팅

## 실행 환경

- **Docker Desktop 필수** — 인프라(Redis×4, Kafka, PostgreSQL)와 서비스(Gateway, Auth, Chat)를 모두 컨테이너로 실행
- **Compose 파일**: `apex_infra/docker/docker-compose.e2e.yml`
- **E2E 테스트 바이너리**: `build/Linux/debug/bin/apex_e2e_tests` (Linux) / `build/Windows/debug/apex_services/tests/e2e/apex_e2e_tests.exe` (Windows)
- **스트레스 테스트 바이너리**: `build/Linux/debug/bin/apex_e2e_stress_tests`
- **설정 파일**: `gateway_e2e.toml`, `auth_svc_e2e.toml`, `chat_svc_e2e.toml`

## 실행 순서

```bash
# 1. 인프라 + 서비스 기동 (--wait로 모든 healthcheck 통과까지 대기)
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --build --wait

# 2. E2E 테스트 실행
./build/Linux/debug/bin/apex_e2e_tests --gtest_output=xml:e2e-results.xml

# 3. 스트레스 테스트 실행 (선택)
./build/Linux/debug/bin/apex_e2e_stress_tests

# 4. 정리 (볼륨까지 완전 삭제)
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

## 환경변수

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `E2E_GATEWAY_HOST` | `127.0.0.1` | Gateway TCP/WebSocket 접속 호스트 |
| `E2E_GATEWAY_TCP_PORT` | `8443` | Gateway TCP 포트 |
| `E2E_STARTUP_TIMEOUT` | `60` (초) | 서비스 기동 대기 상한 |
| `E2E_REQUEST_TIMEOUT` | `30` (초) | 개별 요청 타임아웃 |
| `E2E_STRESS_CONNECTIONS` | `10` | 스트레스 테스트 동시 연결 수 |
| `E2E_STRESS_MESSAGES` | `20` | 스트레스 테스트 연결당 메시지 수 |

## 서비스 프로세스 관계

```
docker-compose.e2e.yml
  ├── kafka          (apache/kafka:4.2.0, KRaft mode)
  ├── redis-auth     (redis:7-alpine, port 6380)
  ├── redis-pubsub   (redis:7-alpine, port 6381)
  ├── redis-ratelimit(redis:7-alpine, port 6382)
  ├── redis-chat     (redis:7-alpine, port 6383)
  ├── postgres       (postgres:16-alpine)
  ├── apex-gateway   (gateway.Dockerfile, port 8443/8444)
  ├── apex-auth-svc  (auth-svc.Dockerfile)
  └── apex-chat-svc  (chat-svc.Dockerfile)

E2E 테스트 바이너리 (apex_e2e_tests)
  └── TcpClient → apex-gateway:8443
```

서비스 프로세스는 Docker 컨테이너로 실행되므로 테스트 바이너리가 직접 기동/종료하지 않는다.

## E2E 테스트 구조

| 파일 | 테스트 수 | 커버리지 |
|------|-----------|----------|
| `e2e_auth_test.cpp` | 3 | JWT 로그인, 미인증 거부, 토큰 갱신 |
| `e2e_chat_test.cpp` | 3 | 방 메시지 브로드캐스트, 방 목록, 글로벌 브로드캐스트 |
| `e2e_ratelimit_test.cpp` | 3 | Per-User, Per-IP, Per-Endpoint rate limit |
| `e2e_timeout_test.cpp` | 2 | 서비스 타임아웃, 타임아웃 후 복구 |

## 스트레스 테스트 구조

`apex_e2e_stress_tests` 바이너리 (12개 테스트):

| 파일 | 테스트 | 설명 |
|------|--------|------|
| `e2e_stress_connection.cpp` | RapidConnectDisconnect | 다수 클라이언트 고속 연결/해제 반복 |
| | HalfOpenConnection | 연결 후 무응답 클라이언트 처리 |
| | DisconnectDuringResponse | 응답 도중 연결 끊김 |
| `e2e_stress_concurrency.cpp` | ConcurrentRoomJoinLeave | 다수 클라이언트 동시 방 입/퇴장 |
| | ConcurrentLogin | 동시 로그인 요청 |
| | RateLimitBurst | 버스트 트래픽 rate limit 동작 |
| `e2e_stress_infra.cpp` | MassTimeouts | 대량 타임아웃 발생 시 서비스 복구 |
| | KafkaReconnect | Kafka 브로커 재연결 내구성 |
| | RedisReconnect | Redis 재연결 내구성 |
| `e2e_stress_protocol.cpp` | IncompleteFrame | 불완전 프레임 전송 처리 |
| | MaxSizeMessage | 최대 크기 메시지 처리 |
| | InvalidMsgIdFlood | 잘못된 msg_id 플러딩 처리 |

## CI 실행 방식

### ci.yml — PR 빌드 후 E2E job

```yaml
# 1. build job: 서비스 바이너리 빌드 (Linux debug preset)
# 2. e2e job: build job 성공 시 실행
#   - 사전 빌드된 바이너리로 서비스 Docker 이미지 빌드
#   - docker compose up -d --wait --timeout 120
#   - apex_e2e_tests 실행
#   - docker compose down -v
```

### nightly.yml — Valgrind + E2E + 스트레스 (cron + workflow_dispatch)

```yaml
# 1. build: linux-debug preset
# 2. valgrind-unit: ctest -LE "e2e|e2e_stress" --overwrite MemoryCheckCommand=valgrind
# 3. e2e: docker compose up + apex_e2e_tests + apex_e2e_stress_tests
```

## 트러블슈팅

### 1. Docker 컨테이너 상태 불일치

**증상**: 이전 실행의 Docker 볼륨이 남아 있어 Kafka 토픽 중복 생성 경고 또는 PostgreSQL 스키마 충돌.

**해결**:
```bash
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d --build --wait
```

### 2. Rate Limit 윈도우 간섭

**증상**: PerIpRateLimit 테스트 이후 PerEndpointRateLimit 또는 TimeoutE2E 테스트가 IP 차단으로 실패.

**원인**: PerIpRateLimit이 50개 요청을 쏟아부어 IP 기반 rate limit을 소진함. 윈도우가 만료되기 전에 다음 테스트가 시작되면 같은 IP에서의 새 연결이 거부됨.

**설정**:
- `gateway_e2e.toml`의 `rate_limit.ip.window_size_seconds`를 짧게 설정 (현재 2초)
- `e2e_ratelimit_test.cpp` PerIpRateLimit 테스트 끝에 `sleep(window_size + 1초)` 추가되어 있음

### 3. PubSub 구독 타이밍

**증상**: RoomMessageBroadcast 테스트에서 bob이 채널 구독 후 alice의 메시지를 못 받음.

**원인**: PubSubListener는 별도 스레드에서 `select()`로 1초 주기 폴링. `subscribe()` 호출 후 실제 Redis SUBSCRIBE 명령이 적용되기까지 최대 1초 + α 지연.

**설정**: 테스트에서 `subscribe_channel()` 후 `std::this_thread::sleep_for(1500ms)` 대기 중. 이 값을 줄이면 간헐적 실패 위험.

### 4. Kafka 브로커 미준비

**증상**: 첫 번째 테스트(보통 AuthE2ETest.LoginAndAuthenticatedRequest)가 타임아웃.

**원인**: Docker Kafka 컨테이너는 healthy 체크를 통과해도 내부 브로커가 토픽 생성을 완료하지 못한 상태일 수 있음.

**해결**: `--wait` 플래그로 기동 시 healthcheck 통과를 기다리지만, 첫 실행 시 Kafka 초기화가 특히 느림. `E2E_STARTUP_TIMEOUT`을 늘리거나 `--timeout 120` 조정.

### 5. GlobalBroadcast 메시지 순서

**증상**: GlobalBroadcast 테스트에서 alice가 response(2042) 전에 broadcast(2043)를 먼저 수신.

**원인**: Redis PubSub PUBLISH가 Kafka 라운드트립보다 빠를 수 있음. 이는 설계상 정상 — 브로드캐스트는 Gateway 내부에서 바로 fanout하고, response는 Kafka를 거쳐 돌아옴.

**테스트 작성 시**: 여러 비동기 메시지를 기대할 때는 순서에 의존하지 말고, 모든 메시지를 수집한 뒤 존재 여부를 검증.

### 6. hiredis 명령 포맷팅 주의

**배경**: RedisMultiplexer의 `command(string_view)` (deprecated)는 hiredis `redisAsyncCommand`에 format string으로 전달. `%` 없으면 공백 기준으로 토큰 분리됨.

**규칙**: 항상 파라미터화된 `command(const char* fmt, Args&&...)` 사용.

```cpp
// BAD — 공백에서 분리됨
multiplexer_.command("EVAL " + lua_script + " 2 key1 key2");

// GOOD — %s가 bulk string 생성
multiplexer_.command("EVAL %s 2 %s %s", script.c_str(), key1.c_str(), key2.c_str());
```
