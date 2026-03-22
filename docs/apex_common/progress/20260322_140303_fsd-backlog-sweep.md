# FSD 백로그 소탕 — v0.5.10.4

**브랜치**: feature/fsd-backlog-20260322_134217
**일시**: 2026-03-22

---

## 해결 (2건)

### BACKLOG-134: AdapterState가 AdapterInterface에 미노출
- `AdapterState` enum을 `apex::shared::adapters` → `apex::core`로 이동
- `AdapterInterface`에 `virtual AdapterState state() const noexcept = 0` 추가
- `AdapterWrapper`에 `state()` 포워딩 구현
- `adapter_base.hpp`에 `using apex::core::AdapterState` alias로 shared 코드 호환 유지
- `test_wire_services.cpp`의 MockAdapter에 `state()` override 추가

### BACKLOG-19: Auth/Chat 비즈니스 로직 세밀 테스트 부족
- **auth_logic.hpp**: `is_account_locked(locked_until_str, now)` 순수 함수 추출
  - PostgreSQL timestamptz 문자열 vs UTC 시간 비교 로직
  - auth_service.cpp 핸들러에서 인라인 코드를 함수 호출로 교체
- **chat_logic.hpp**: 3개 constexpr 순수 함수 추출
  - `validate_message_content(size, max)` → MessageValidation enum
  - `validate_room_name(size, max)` → RoomNameValidation enum
  - `interpret_join_result(lua_result)` → JoinRoomResult enum
  - chat_service.cpp의 on_create_room, on_join_room, on_send_message, on_whisper에서 호출로 교체
- **신규 테스트 24건**:
  - test_crypto_util.cpp (5): sha256_hex 4건 + generate_secure_token 4건
  - test_auth_logic.cpp (7): is_account_locked 경계값 테스트
  - test_chat_logic.cpp (12): 메시지/방이름 검증 + join 결과 해석 + 엣지 케이스

## 드롭 (4건)

| 항목 | 사유 |
|------|------|
| BACKLOG-102 | interface 추출 선행 필요 (JwtVerifier, JwtBlacklist, RateLimitFacade → 가상 인터페이스) |
| BACKLOG-127 | #102와 동일 — interface 추출 선행 필요 |
| BACKLOG-133 | Transport concept 타입 소거 재설계 필요, TLS Listener 구현과 연동 |
| BACKLOG-135 | v0.6 운영 인프라 마일스톤에서 시크릿 관리 방식 결정 후 착수 |

## 빌드 결과

- 82/82 테스트 전체 통과 (신규 3개 테스트 타겟 포함)
- Zero Warning (MSVC /W4 /WX)
