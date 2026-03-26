# Whisper O(1) Core Routing 완료 (BACKLOG-149)

- **PR**: #208
- **브랜치**: feature/whisper-core-routing

## 변경 요약

Whisper unicast 전송 시 `core_id=0` 하드코딩을 제거하여 Gateway ResponseDispatcher가 모든 코어를 순회(O(N_cores))하던 비효율을 O(1) 단일 코어 직접 전달로 개선.

## 변경 파일 (5개 코드 + 1개 설계)

| 파일 | 변경 |
|------|------|
| `auth-svc/session_store.hpp/cpp` | `set_user_session_id`에 `core_id` 파라미터 추가, Redis 값 `"sid:core_id"` 합성 |
| `auth-svc/auth_service.cpp` | on_login/on_refresh에서 `meta.core_id` 전달 |
| `chat-svc/chat_service.cpp` | `parse_session_core` 헬퍼 + on_whisper 파싱/라우팅 변경 |
| `gateway/response_dispatcher.cpp` | corr_id==0 경로에 단일 코어 post 분기 + 폴백 |

## 하위 호환

- 레거시 Redis 값(`:` 없음) → `core_id=0` 폴백
- Gateway에서 core_id 범위 초과 시 → 전수 검색 폴백

## auto-review

MINOR 1건(디버그 로그 추가) 수정 완료.

## 빌드/테스트

- 로컬: MSVC debug 빌드 성공, 97 테스트 통과
- CI: 3개 컴파일러 (MSVC + GCC + Clang) 검증
