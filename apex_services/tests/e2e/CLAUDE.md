# E2E 테스트 — 실행 가이드 & 트러블슈팅

## 실행 환경

- **Docker Desktop 필수** — 6개 컨테이너: Redis Auth/PubSub/RateLimit/Chat, Kafka, PostgreSQL
- **Compose 파일**: `apex_infra/docker/docker-compose.e2e.yml`
- **테스트 바이너리**: `build/Windows/debug/apex_services/tests/e2e/apex_e2e_tests.exe`
- **설정 파일**: `gateway_e2e.toml`, `auth_svc_e2e.toml`, `chat_svc_e2e.toml`

## 실행 순서

```bash
# 1. Docker 인프라 기동
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d

# 2. 컨테이너 healthy 대기 (Kafka 브로커 초기화에 5~10초)
sleep 5

# 3. E2E 테스트 실행 (테스트 fixture가 Gateway/Auth/Chat 서비스를 자동 기동·종료)
D:/.workspace/.worktrees/e2e-fix/build/Windows/debug/apex_services/tests/e2e/apex_e2e_tests.exe

# 4. Docker 인프라 정리 (테스트 fixture가 compose down도 수행하지만, 수동 정리가 필요할 때)
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
```

## 트러블슈팅

### 1. 좀비 프로세스 — Gateway/AuthService/ChatService

**증상**: E2E 테스트가 비정상 종료(크래시, Ctrl+C 등)하면 Gateway·AuthService·ChatService 프로세스가 좀비로 남아 다음 실행 시 포트 충돌 발생.

**해결**:
```bash
# Windows — 좀비 프로세스 확인 및 정리
tasklist | grep -E "apex_gateway|apex_auth|apex_chat"
taskkill //F //IM apex_gateway.exe 2>/dev/null
taskkill //F //IM apex_auth_service.exe 2>/dev/null
taskkill //F //IM apex_chat_service.exe 2>/dev/null
```

**예방**: 테스트 fixture의 `TearDown()`이 정상 종료 시 프로세스를 kill하지만, 비정상 종료 시에는 동작하지 않음. 테스트 실행 전에 위 명령으로 확인 습관 필요.

### 2. Docker 컨테이너 상태 불일치

**증상**: 이전 실행의 Docker 볼륨이 남아 있어 Kafka 토픽 중복 생성 경고 또는 PostgreSQL 스키마 충돌.

**해결**:
```bash
# 볼륨까지 완전 삭제 후 재기동
docker compose -f apex_infra/docker/docker-compose.e2e.yml down -v
docker compose -f apex_infra/docker/docker-compose.e2e.yml up -d
```

### 3. Rate Limit 윈도우 간섭

**증상**: PerIpRateLimit 테스트 이후 PerEndpointRateLimit 또는 TimeoutE2E 테스트가 IP 차단으로 실패.

**원인**: PerIpRateLimit이 50개 요청을 쏟아부어 IP 기반 rate limit을 소진함. 윈도우가 만료되기 전에 다음 테스트가 시작되면 같은 IP에서의 새 연결이 거부됨.

**설정**:
- `gateway_e2e.toml`의 `rate_limit.ip.window_size_seconds`를 짧게 설정 (현재 2초)
- `e2e_ratelimit_test.cpp` PerIpRateLimit 테스트 끝에 `sleep(window_size + 1초)` 추가되어 있음
- **CI에서 TSAN 등 느린 빌드**: 윈도우가 2초여도 테스트 자체가 느려지면 문제 없음 (윈도우 내 요청 수가 줄어들기 때문). 다만 sleep 시간은 절대 시간이라 CI에서도 유효.

### 4. PubSub 구독 타이밍

**증상**: RoomMessageBroadcast 테스트에서 bob이 채널 구독 후 alice의 메시지를 못 받음.

**원인**: PubSubListener는 별도 스레드에서 `select()`로 1초 주기 폴링. `subscribe()` 호출 후 실제 Redis SUBSCRIBE 명령이 적용되기까지 최대 1초 + α 지연.

**설정**: 테스트에서 `subscribe_channel()` 후 `std::this_thread::sleep_for(1500ms)` 대기 중. 이 값을 줄이면 간헐적 실패 위험.

### 5. Kafka 브로커 미준비

**증상**: 첫 번째 테스트(보통 AuthE2ETest.LoginAndAuthenticatedRequest)가 타임아웃.

**원인**: Docker Kafka 컨테이너는 healthy 체크를 통과해도 내부 브로커가 토픽 생성을 완료하지 못한 상태일 수 있음.

**해결**: `docker compose up -d` 후 5~10초 대기. 테스트 fixture에서도 health check 로직이 있지만, 첫 실행 시 Kafka 초기화가 특히 느림.

### 6. GlobalBroadcast 메시지 순서

**증상**: GlobalBroadcast 테스트에서 alice가 response(2042) 전에 broadcast(2043)를 먼저 수신.

**원인**: Redis PubSub PUBLISH가 Kafka 라운드트립보다 빠를 수 있음. 이는 설계상 정상 — 브로드캐스트는 Gateway 내부에서 바로 fanout하고, response는 Kafka를 거쳐 돌아옴.

**테스트 작성 시**: 여러 비동기 메시지를 기대할 때는 순서에 의존하지 말고, 모든 메시지를 수집한 뒤 존재 여부를 검증.

### 7. hiredis 명령 포맷팅 주의

**배경**: RedisMultiplexer의 `command(string_view)` (deprecated)는 hiredis `redisAsyncCommand`에 format string으로 전달. `%` 없으면 공백 기준으로 토큰 분리됨.

**영향**: Lua 스크립트처럼 공백/줄바꿈이 포함된 인자를 보내면 파싱이 깨짐.

**규칙**: 항상 파라미터화된 `command(const char* fmt, Args&&...)` 사용. `%s`가 hiredis RESP bulk string을 올바르게 생성.

```cpp
// BAD — 공백에서 분리됨
multiplexer_.command("EVAL " + lua_script + " 2 key1 key2");

// GOOD — %s가 bulk string 생성
multiplexer_.command("EVAL %s 2 %s %s", script.c_str(), key1.c_str(), key2.c_str());
```

## 테스트 구조

| 파일 | 테스트 수 | 커버리지 |
|------|-----------|----------|
| `e2e_auth_test.cpp` | 3 | JWT 로그인, 미인증 거부, 토큰 갱신 |
| `e2e_chat_test.cpp` | 3 | 방 메시지 브로드캐스트, 방 목록, 글로벌 브로드캐스트 |
| `e2e_ratelimit_test.cpp` | 3 | Per-User, Per-IP, Per-Endpoint rate limit |
| `e2e_timeout_test.cpp` | 2 | 서비스 타임아웃, 타임아웃 후 복구 |

## 서비스 프로세스 관계

```
E2E fixture (GTest)
  ├── docker compose up -d  (Redis×4 + Kafka + PostgreSQL)
  ├── Gateway (ws:8444, tcp:8443) → Kafka → Redis Auth/RateLimit
  ├── AuthService → Kafka → Redis Auth
  ├── ChatService → Kafka → Redis Chat + PostgreSQL
  └── TcpClient (테스트 코드) → Gateway:8443
```

테스트 fixture가 Gateway/AuthService/ChatService를 `CreateProcessW`로 기동하고, 테스트 종료 시 `TerminateProcess`로 정리.
