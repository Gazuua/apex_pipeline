# BACKLOG HISTORY

완료된 백로그 항목 아카이브. 최신 항목이 파일 상단.
모든 이슈는 BACKLOG.md 경유 필수 — 히스토리 직접 생성 금지.

<!-- NEW_ENTRY_BELOW -->

### #1. apex_core 프레임워크 가이드 (API 가이드 + 내부 아키텍처)
- **등급**: CRITICAL | **스코프**: core, docs | **타입**: docs
- **해결**: 2026-03-19 12:47:03 | **방식**: DOCUMENTED
- **비고**: 단일 파일 2레이어 구조 (`docs/apex_core/apex_core_guide.md`). 레이어 1(§1-§9): 서비스 스켈레톤, ServerConfig, 라이프사이클 Phase 1-3.5, 핸들러 4종, 어댑터, 메모리, 유틸리티, 금지사항 7종 BAD/GOOD, 빌드 CMake 템플릿. 레이어 2(§10): 컴포넌트 배치도, 요청/Kafka 흐름, ADR 포인터 10개. 부록(§11): 실전 패턴 4종. 아키텍처 결정 D1-D7 (#48 핸드오프 기반) 포함 — 코드 구현은 #48 담당.

### #60. 로그 디렉토리 구조 확립 + 경로 중앙화 + 파일명 표준화
- **등급**: MAJOR | **스코프**: core, gateway, auth-svc, chat-svc, infra | **타입**: infra
- **해결**: 2026-03-19 10:56:29 | **방식**: FIXED
- **비고**: async logger + daily_file_format_sink + exact_level_sink 조합. 서비스별/레벨별/날짜별 로그 분리, 프로젝트 루트 자동 탐지, service_name 검증, E2E 로그 경로 통합. 71/71 유닛 통과.

### #55. 로컬 빌드 큐잉 + 머지 직렬화 시스템 (Windows)
- **등급**: MAJOR | **스코프**: tools, infra | **타입**: infra
- **해결**: 2026-03-19 00:49:44 | **방식**: FIXED
- **비고**: `queue-lock.sh` 통합 스크립트 (FIFO 큐 + mkdir atomic lock + PID/timestamp stale 감지). PreToolUse hook으로 빌드/머지 우회 원천 차단. 테스트 26/26 + 빌드 71/71 통과.

### #15. Server 라이프사이클 에러 경로 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: test_server_error_paths.cpp에 5개 TC 구현 완료 (포트 점유, 이중 run, shutdown, 재진입, listener 없는 run).

### #12. Server 예외 경로 소멸 순서
- **등급**: MAJOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:21:52 | **방식**: WONTFIX
- **비고**: Listener::start() 로컬 벡터 → move-assign 패턴으로 RAII 이미 적용. 중간 상태 불가능.

### #17. Gateway/Auth/Chat 핵심 모듈 테스트 부재
- **등급**: MAJOR | **스코프**: gateway, auth-svc, chat-svc | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: MockKafkaAdapter/MockPgPool 구축. test_message_router, test_auth_handlers, test_chat_handlers 구현 완료.

### #18. Mock thread-safety 불일치
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 22:21:52 | **방식**: FIXED
- **비고**: MockKafkaAdapter, MockPgPool 모두 std::mutex + std::lock_guard 적용 확인.

### #37. cross-thread acceptor close
- **등급**: MINOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:14:17 | **방식**: WONTFIX
- **비고**: Boost.Asio acceptor.close()는 스레드 안전. atomic running_ 플래그로 이중 호출 방지. 실제 버그 아님.

### #28. drain_timeout 만료 시 Server 멤버 소멸 순서
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 22:14:17 | **방식**: WONTFIX
- **비고**: C++ 멤버 선언 역순 소멸(RAII)으로 안전. shutdown_timer_ unique_ptr 자동 정리. 추가 조치 불필요.

### #35. test_redis_reply.cpp 매직 넘버 0
- **등급**: MINOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: nullptr reply type=0에 의도 설명 주석 추가.

### #34. test_session.cpp 매직 넘버 256 하드코딩
- **등급**: MINOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: kDefaultMaxQueueDepth named constant 도입.

### #33. vcpkg.json 의존성 정리 + 버전 불일치
- **등급**: MINOR | **스코프**: infra | **타입**: infra
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: version-semver 0.4.0 → 0.5.5 갱신. boost-beast는 v0.6 계획상 유지(#38).

### #31. make_socket_pair 반환 순서 불일치
- **등급**: MINOR | **스코프**: core | **타입**: bug
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: bench_helpers.hpp 반환 순서를 {server, client}로 통일. 호출부(bench_session_throughput.cpp) 동기 수정.

### #25. GatewayEnvelope FBS msg_id uint16 불일치
- **등급**: MINOR | **스코프**: gateway, shared | **타입**: design-debt
- **해결**: 2026-03-18 22:11:38 | **방식**: FIXED
- **비고**: gateway_envelope.fbs msg_id를 uint16 → uint32로 수정. C++ 코드와 일치.

### #45. plans-progress 추적성 갭
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:48:57 | **방식**: SUPERSEDED
- **비고**: #59(문서 자동화)에 흡수. 레거시 소급 보정은 ROI 없음, 향후 pre-commit hook으로 강제 예정.

### #46. auto-review 리뷰어 확장
- **등급**: MINOR | **스코프**: tools | **타입**: infra
- **해결**: 2026-03-18 21:44:31 | **방식**: SUPERSEDED
- **비고**: 5→6→12→7명으로 진화 후 plugin 체제 최종 안착. 현재 7명 리뷰어가 v0.6까지 충분.

### 전체 문서에서 특정 표현 완전 제거
- **등급**: MAJOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: 대상 16건 전수 치환 완료. 7개 파일 수정. grep 제로 확인 완료.

### 백로그 연관 링킹 필드 + 섹션 내 우선순위 규칙 + NOW 기준 확장
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: docs/CLAUDE.md § 백로그 운영에 3건 반영 완료. 기존 연관 필드 양방향 검증 완료.

### CI docs-only 커밋 빌드 스킵
- **등급**: MAJOR | **스코프**: ci | **타입**: infra
- **해결**: 2026-03-18 21:10:25 | **방식**: FIXED
- **비고**: dorny/paths-filter@v3 이미 구현됨 (.github/workflows/ci.yml). source 필터로 docs-only PR 자동 스킵.

### session.cpp clang-tidy 워닝 잔여분
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 19:57:43 | **방식**: SUPERSEDED
- **비고**: #54 (빌드/정적분석 경고 전수 소탕)에 흡수.

### main 히스토리 문서 전용 커밋 squash
- **등급**: MINOR | **스코프**: docs | **타입**: infra
- **해결**: 2026-03-18 12:53:47 | **방식**: SUPERSEDED
- **비고**: --squash merge 워크플로우가 이미 PR 단위로 처리. interactive rebase on main은 안전 규칙 위반.

### 테스트 이름 오타 MoveConstruction 2건
- **등급**: MINOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: MoveConstruction은 정상 영어 (Move + Construction). 오타 아님.

### Compaction / LSA (Log-Structured Allocator)
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: 현 아키텍처(bump+slab+arena)에서 외부 단편화 거의 없어 구조적 불필요. GB급 인메모리 캐시 도입 시만 재평가.

### new_refresh_token E2E 테스트 미검증
- **등급**: MAJOR | **스코프**: auth-svc | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: 98eca92
- **비고**: e2e_auth_test.cpp에서 token refresh flow + new_refresh_token 필드 검증 완료. 11/11 E2E 통과.

### Session async_recv 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_session.cpp에 10+ 시나리오 (정상 read, frame buffering, EOF, 에러). 71/71 유닛 통과.

### RedisMultiplexer 코루틴 명령 테스트
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_redis_adapter.cpp에서 async 명령 실행 + 에러 처리 검증 완료.

### ConnectionHandler 단위 테스트 부재
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_connection_handler.cpp 추가. 9+ 테스트 (accept, read loop, dispatch, session lifecycle, multi-listener). 71/71 통과.

### review 문서 2개 상세 내용 부재
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: v0.5 Wave 1 review 원본 데이터 없이 복원 불가. 초기 레거시.

### E2E 테스트 실행 가이드 문서
- **등급**: MAJOR | **스코프**: docs, infra | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: apex_services/tests/e2e/CLAUDE.md에 Docker 셋업, 서비스 라이프사이클, 트러블슈팅 6섹션, 테스트 매트릭스 완성.

### ResponseDispatcher 하드코딩 오프셋
- **등급**: MINOR | **스코프**: gateway | **타입**: bug
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: df33f60
- **비고**: envelope_payload_offset() 함수 호출로 동적 계산으로 교체.

### 별도 백로그 파일 2건 미이전
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: backlog_memory_os_level.md + 20260315_094300_backlog.md → BACKLOG.md 통합 완료, 원본 삭제.

---
