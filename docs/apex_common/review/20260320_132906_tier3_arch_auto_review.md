# Auto-Review: Tier 3 아키텍처 정비 (v0.5.9.0)

**브랜치**: `feature/backlog-tier3-arch`
**리뷰 일시**: 2026-03-20
**모드**: task (git diff main...HEAD)
**변경 규모**: 81 files, +3446/-768

---

## 리뷰어 디스패치

| 리뷰어 | 관점 | 소요 시간 |
|--------|------|-----------|
| reviewer-design | API/모듈 경계/타입 안전성 | ~4.5분 |
| reviewer-logic | 에러 흐름/상태 전이/엣지 케이스 | ~3.7분 |
| reviewer-systems | 메모리/동시성/코루틴 수명 | ~4.7분 |
| reviewer-test | 테스트 커버리지/정확성 | ~3.2분 |

---

## 발견 이슈 요약

### 즉시 수정 (5건)

| # | 등급 | 리뷰어 | 파일 | 설명 | 조치 |
|---|------|--------|------|------|------|
| 1 | CRITICAL | design | `connection_handler.hpp` | ServiceError 반환 시 connection_handler가 이중 에러 프레임 전송 가능 (미래 서비스) | ServiceError 분기 추가 — 에러 프레임 전송 스킵 |
| 2 | MAJOR | design | `service_base.hpp` | spawn() fetch_sub `release` vs spawn_tracked `acq_rel` memory ordering 비대칭 | `acq_rel`로 통일 |
| 3 | MAJOR | design | `error_code.hpp` | ErrorCode 100-999 구간 용도 불명확, ServiceError semantic 미문서화 | 범위 가이드 + semantic 주석 추가 |
| 4 | MINOR | systems+design | `session.hpp` | `#include <fmt/format.h>` 파일 중간 배치 | 상단으로 이동 |
| 5 | MINOR | systems | `core_engine.hpp` | spawn_tracked catch(const std::exception&)만 — non-std 예외 누락 | catch(...) 추가 |

### 백로그 등록 (4건)

| # | 등급 | 리뷰어 | BACKLOG | 설명 |
|---|------|--------|---------|------|
| 6 | MAJOR | logic | #100 | Blacklist fail-open 보안 정책 — Redis 장애 시 탈취 토큰 허용 위험 |
| 7 | MAJOR | test | #101 | build_error_frame service_error_code 라운드트립 테스트 부재 |
| 8 | MAJOR | test | #102 | GatewayPipeline 에러 흐름 단위 테스트 부재 |
| 9 | MINOR | design | #103 | KafkaMessageMeta.session_id SessionId 강타입화 미반영 |

### 문제 없음 확인 (INFO 5건)

| 리뷰어 | 항목 | 결론 |
|--------|------|------|
| logic | "direct send + ok()" 패턴 | 이중 에러 프레임 없음 확인 |
| logic | AuthState 상태 전이 | 올바름 — 매 요청 재검증 |
| logic | MetadataPrefix→KafkaMessageMeta 변환 | 1:1 정확 |
| systems | 코루틴 lifetime (send_error 람다) | 코루틴 프레임이 수명 보장 |
| systems | KafkaAdapter spawn_tracked 전환 | 캡처 수명 올바름 |

---

## 잘된 설계 포인트 (design 리뷰어)

1. **core→shared 역방향 의존 완전 해소** — forwarding header 제거, WireHeader/FrameCodec core 복원
2. **ServiceError sentinel + service_error_code 패턴** — 서비스 추가 시 core 변경 불필요
3. **KafkaMessageMeta 도입** — core의 kafka_envelope 의존 제거, 경계 깔끔
4. **GatewayService 리팩터링** — 거대 람다 → 4개 명명 메서드 분리
5. **FrameType concept** — payload() accessor 제약, static_assert 컴파일 타임 검증
6. **ServiceBase post()/get_executor()** — io_context 캡슐화

---

## 빌드 검증

- 수정 후 빌드 진행 중 (MSVC `/W4 /WX`)
- clang-format 적용 완료
