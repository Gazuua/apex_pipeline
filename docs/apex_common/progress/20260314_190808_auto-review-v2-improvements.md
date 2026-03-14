# auto-review v2.0 시스템 개편 + full 리뷰 완료 기록

**작성일**: 2026-03-14
**브랜치**: feature/auto-review-test
**리뷰 보고서**: `docs/apex_common/review/20260314_190808_auto-review.md`

---

## 완료 항목

### auto-review v2.0 시스템 개편
- [x] 3계층 팀 구조 개편 — 스폰 책임 메인 이동, SendMessage 기반 관리
- [x] 에이전트 경로 정합성 — 워크트리 기반 경로 참조 일관성 확보
- [x] start 시그널 대기 — 리뷰어 준비 완료 후 일괄 시작
- [x] 라운드별 커밋 — 라운드 수정사항 자동 커밋 로직 추가
- [x] 팀 해산 로직 — Clean 달성 시 리뷰어 정리
- [x] 에스컬레이션 정책 — 자체수정 + 백로그 정책 명시
- [x] 리뷰어 자율성 — 12개 리뷰어 파일에 자율 판단 가이드 명시

### full 모드 리뷰 결과
- [x] 코드 리뷰 이슈 수정 (41건) — 코드 5건 + 문서 36건
  - pg_config.hpp password 하드코딩 제거 (Critical)
  - cross_core_dispatcher.cpp register_handler assert 추가
  - kafka_sink.cpp null-terminator 의존 제거 + JSON 이스케이프
  - pg_connection.cpp std::atoi → std::strtol 전환
  - pgbouncer/userlist.txt 로컬 개발 전용 경고 주석
  - apex_docs/ 잔존 경로 → docs/ 치환 (10파일 21건)
  - progress 미완료 체크박스 완료 처리 (3파일 10건)
  - README/docs 정합성 수정 (5건)
- [x] 백로그 문서 작성 — v0.5 스코프 8건 연기 (Critical 2 + Important 1 + Minor 5)
- [x] 빌드 + 테스트 통과 — 165 컴파일 + 45 테스트 100% 통과
- [x] CLAUDE.md 업데이트 — 빌드 명령어 + 에이전트 규칙 추가

### Clean 도메인 (6/11)
- [x] architecture
- [x] memory
- [x] api
- [x] test-coverage
- [x] test-quality
- [x] infra

---

## 알려진 이슈 (수정 보류)

| 이슈 | 이유 | 대상 버전 |
|------|------|----------|
| RedisMultiplexer PendingCommand UAF | heap 할당 전환 필요, 구조적 리팩토링 | v0.5 |
| RedisMultiplexer on_disconnect 미구현 | 재연결 로직 설계 필요 | v0.5 |
| Redis AUTH 미구현 | 인증 프로토콜 추가 필요 | v0.5 |
| RedisMultiplexer privdata FIFO 가정 | UAF와 동시 수정 권장 | v0.5 |
| RedisReply ARRAY 타입 미지원 | SMEMBERS/LRANGE 파싱 불가 | v0.5 |
| PgTransaction begun_ unit test 불가 | mock PgConnection 필요 | v0.5 |
| RedisMultiplexer pipeline() 순차 실행 | 진짜 파이프라이닝 별도 설계 | v0.5 |
| Server 라이프사이클 에러 경로 테스트 | 별도 태스크 | v0.5 |
