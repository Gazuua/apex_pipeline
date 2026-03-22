# FSD Backlog Sweep — 완료 기록

**브랜치**: feature/fsd-backlog-20260322_105023
**일시**: 2026-03-22
**버전**: v0.5.10.2

---

## 해결 항목 (6건)

| # | 항목 | 타입 | 해결 방식 |
|---|------|------|----------|
| #128 | AuthService locked_until 시간 비교 | bug | FIXED (이전 PR #96에서 해결, 히스토리 이전) |
| #129 | RedisMultiplexer reconnect_loop 생존 보장 | design-debt | FIXED — member backoff_timer_ + 소멸자 cancel |
| #26 | ReplyTopicHeader::serialize() silent failure | design-debt | FIXED — std::expected 반환 + TopicTooLong 에러 |
| #27 | FrameError→ErrorCode 매핑 구분 불가 | design-debt | FIXED — UnsupportedProtocolVersion 추가 + BodyTooLarge→BufferFull |
| #38 | boost-beast 미사용 의존성 | infra | DOCUMENTED — vcpkg.json $comment로 의도 명시 |
| #39 | CMakeLists.txt 하드코딩 경로 | infra | FIXED — APEX_CORE_BIN_DIR 변수 통일 |

## 드롭 항목 (FSD 분석 메모 완료)

IN VIEW 10건, DEFERRED 5건에 `[FSD 분석 2026-03-22]` 메모 부착.
자동화 불가 사유: 설계 판단 필요, 특정 환경 필요, 선행 작업 의존 등.

## auto-review 결과

리뷰어 4명 디스패치. 로직 결함 0건.
테스트 누락 2건 + MockProtocol 동기화 1건 수정.
BACKLOG-132 신규 등록 (RedisAdapter::do_close() close() 미호출).

## 변경 파일 (최종)

- 코어: frame_codec.hpp/cpp, error_code.hpp(미변경), wire_header.hpp(미변경)
- 프로토콜: tcp_binary_protocol.hpp, kafka_envelope.hpp/cpp
- 어댑터: redis_multiplexer.hpp/cpp
- 인프라: vcpkg.json, CMakeLists.txt ×3
- 테스트: test_tcp_binary_protocol.cpp, test_kafka_envelope.cpp, test_mocks.hpp
- 문서: BACKLOG.md, BACKLOG_HISTORY.md
