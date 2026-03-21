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

## 트러블슈팅

### 1. Docker 컨테이너 상태 불일치
이전 볼륨 잔존 → Kafka 토픽 중복/PG 스키마 충돌. `docker compose ... down -v` 후 `up -d --build --wait`로 해결.

### 2. Rate Limit 윈도우 간섭
PerIpRateLimit 후 다른 테스트 IP 차단 실패 — 윈도우 미만료. `gateway_e2e.toml`의 `window_size_seconds` (현재 2초) + 테스트 끝 sleep으로 대응됨.

### 3. PubSub 구독 타이밍
RoomMessageBroadcast에서 구독 후 메시지 미수신 — PubSubListener 폴링 지연 (최대 1초+α). 테스트에서 `subscribe_channel()` 후 1500ms sleep으로 대응. 줄이면 간헐적 실패 위험.

### 4. Kafka 브로커 미준비
첫 테스트 타임아웃 — Kafka healthy 통과 후에도 토픽 생성 미완료. `E2E_STARTUP_TIMEOUT` 또는 `--timeout 120` 조정.

### 5. GlobalBroadcast 메시지 순서
broadcast가 response보다 먼저 도착 가능 — Redis PubSub이 Kafka 라운드트립보다 빠름 (설계상 정상). 테스트에서 메시지 순서 의존 금지, 존재 여부로 검증.

### 6. hiredis 명령 포맷팅 주의
`command(string_view)` (deprecated)는 공백 기준 토큰 분리됨. 항상 파라미터화된 `command(const char* fmt, Args&&...)` 사용:
```cpp
// BAD:  multiplexer_.command("EVAL " + lua_script + " 2 key1 key2");
// GOOD: multiplexer_.command("EVAL %s 2 %s %s", script.c_str(), key1.c_str(), key2.c_str());
```
