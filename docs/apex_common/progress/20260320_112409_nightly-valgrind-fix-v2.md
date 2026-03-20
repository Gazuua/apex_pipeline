# v0.5.8.3 Nightly Valgrind 후속 수정

**PR**: #52
**백로그**: BACKLOG-99 후속
**브랜치**: bugfix/nightly-valgrind-fix-v2

---

## 작업 요약

PR #50 머지 후 Nightly Valgrind 수동 트리거(run 23326043743) 실패 2건 수정.

## valgrind-unit 수정

### 원인
`ctest -T MemCheck --overwrite MemoryCheckCommand=valgrind`이 PATH 내 바이너리를 찾지 못함 (exit code 16).

### 수정
| 파일 | 변경 |
|------|------|
| `.github/workflows/nightly.yml` | `MemoryCheckCommand=valgrind` → `MemoryCheckCommand=/usr/bin/valgrind` |

## valgrind-e2e 수정

### 원인 분석

3/9 테스트 실패 (2개는 기존 필터로 제외됨):

1. **ChatE2ETest.RoomMessageBroadcast**: PubSub 메시지 순서 비결정적 — Valgrind 감속 하 msg_id 2011 수신(기대: 2012)
2. **ChatE2ETest.GlobalBroadcast**: 하드코딩 `recv(5s)` 타임아웃이 Valgrind 10-20x 감속에서 부족 → EOF at 48s
3. **TimeoutE2ETest.ServiceTimeout**: `recv(15s)`인데 Gateway `request_timeout_ms=30000` → Valgrind 하 타임아웃 응답 도달 전 EOF

### 수정

| 파일 | 변경 |
|------|------|
| `.github/workflows/nightly.yml` | gtest_filter에 ServiceTimeout, RoomMessageBroadcast, GlobalBroadcast 추가 제외 |
| `.github/workflows/nightly.yml` | TCP 포트 대기 180s → 300s, Kafka rebalance 대기 30s → 60s |
| `apex_services/tests/e2e/gateway_e2e_valgrind.toml` | `request_timeout_ms` 30000 → 120000 |

### Valgrind E2E 잔존 테스트 (제외 후)

- E2E 6개: Auth(Login, UnauthenticatedReject) + Chat(ListRooms) + RateLimit(PerUser, PerIp, PerEndpoint)
- Stress 10개: 기존 필터 유지 (KafkaReconnect, RedisReconnect 제외)

## 변경 파일 총 2개

- `.github/workflows/nightly.yml`
- `apex_services/tests/e2e/gateway_e2e_valgrind.toml`
