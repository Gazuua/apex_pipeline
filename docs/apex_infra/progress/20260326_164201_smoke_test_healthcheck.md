# smoke test Chat Service 타임아웃 수정 (BACKLOG-241)

- **PR**: #197
- **브랜치**: `bugfix/smoke-test-chat-timeout`
- **일시**: 2026-03-26

## 문제

main CI smoke test에서 `AuthE2ETest.LoginAndAuthenticatedRequest` 실패.
Gateway가 Chat Service Kafka 컨슈머 미준비 상태에서 5s 타임아웃, `ServiceError(99)` 반환.

**근본 원인**: `docker-compose.smoke.yml`의 auth-svc/chat-svc에 healthcheck 미설정 → `--wait`가 컨테이너 시작만 확인, Kafka 컨슈머 초기화 완료를 보장하지 못함.

## 수정

| 레이어 | 변경 | 효과 |
|--------|------|------|
| Docker | 서비스 healthcheck `/ready` 추가 | `--wait`가 Server 초기화 완료까지 대기 |
| Docker | `curl` 패키지 추가 | healthcheck CLI 지원 |
| TOML | `[metrics]` 활성화 | `/ready` 엔드포인트 전제조건 |
| E2E | Chat Service warmup 프로브 | 네이티브 환경에서도 준비 상태 보장 |

## 검증

- 로컬 빌드 + 95/95 유닛 테스트 통과
- CI 8개 잡 전체 통과 (smoke-test는 main-only → 머지 후 검증)
- auto-review 이슈 0건
