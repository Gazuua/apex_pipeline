# Batch B — 코어/공유 단위 테스트 소탕

- **브랜치**: `feature/backlog-67-101-13-14-16-core-shared-unit-tests`
- **일시**: 2026-03-21 18:15:55
- **백로그**: BACKLOG-67, BACKLOG-101, BACKLOG-13, BACKLOG-14, BACKLOG-16
- **버전**: v0.5.10.0 (변경 없음 — 테스트 전용)

---

## 작업 요약

코어(apex_core) 및 공유(apex_shared) 영역의 단위 테스트 갭 5건을 일괄 소탕. 기존 71개 TC에서 79개 TC로 확대 (신규 TC가 기존 테스트 파일에 편입된 건 포함).

---

## 변경 내역

### 파일 변경: 7 new, 5 modified

**신규 파일 (7)**
- `test_server_global.cpp` — server.global<T>() 타입 소거 + 중복 호출 검증 (4 TC)
- `test_consumer_payload_pool.cpp` — ConsumerPayloadPool thread-safe acquire/release + 풀 고갈 fallback (7 TC)
- `test_wire_services.cpp` — wire_services() 서비스 자동 감지 (2 TC)
- `test_error_sender.cpp` — ErrorSender build_error_frame service_error_code FlatBuffers 라운드트립 (4 TC)
- `test_listener.cpp` — Listener<P> start/drain/stop 라이프사이클 (7 TC)
- `test_websocket_protocol.cpp` — WebSocketProtocol try_decode/consume_frame (7 TC)
- `test_pg_transaction.cpp` — PgTransaction begun_ 경로, MockPgConn 기반 (13 TC)

**수정 파일 (5)**
- `pg_transaction.hpp` — 템플릿 리팩터링, pool_ 미사용 멤버 제거, finished_ 가드 추가
- `apex_core/tests/CMakeLists.txt` — 신규 테스트 타겟 등록
- `apex_shared/tests/CMakeLists.txt` — 신규 테스트 타겟 등록
- 설계 문서 2건 — 파일 카운트 오류 수정, 작성일 추가

---

## 백로그 항목별 결과

| 백로그 | 제목 | TC 수 | 상태 |
|--------|------|-------|------|
| #67 | server.global<T>() / ConsumerPayloadPool / wire_services() | 13 TC | 완료 |
| #101 | ErrorSender service_error_code 라운드트립 | 4 TC | 완료 |
| #13 | Listener<P> 라이프사이클 | 7 TC | 완료 |
| #14 | WebSocketProtocol try_decode/consume_frame | 7 TC | 완료 |
| #16 | PgTransaction begun_ 경로 | 13 TC | 완료 |

---

## Auto-Review 결과

- **Fix**: 10건 (test 5 + design 3 + docs-records 2)
- **Informational**: 4건
- **주요 설계 수정**: PgTransaction pool_ 미사용 멤버 제거 (MAJOR), finished_ 가드 추가 (MAJOR)
- **잔여 이슈**: 0건

---

## 테스트 결과

- **79/79 유닛 테스트 통과** (기존 71 → 79)
- 버전 변경 없음 (테스트 전용 작업)
