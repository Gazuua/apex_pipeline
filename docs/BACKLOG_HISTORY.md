# BACKLOG HISTORY

완료된 백로그 항목 아카이브. 최신 항목이 파일 상단.

<!-- NEW_ENTRY_BELOW -->

### #H12. session.cpp clang-tidy 워닝 잔여분
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 19:57:43 | **방식**: SUPERSEDED
- **비고**: #54 (빌드/정적분석 경고 전수 소탕)에 흡수.

### #H11. main 히스토리 문서 전용 커밋 squash
- **등급**: MINOR | **스코프**: docs | **타입**: infra
- **해결**: 2026-03-18 12:53:47 | **방식**: SUPERSEDED
- **비고**: --squash merge 워크플로우가 이미 PR 단위로 처리. interactive rebase on main은 안전 규칙 위반.

### #H10. 테스트 이름 오타 MoveConstruction 2건
- **등급**: MINOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: MoveConstruction은 정상 영어 (Move + Construction). 오타 아님.

### #H9. Compaction / LSA (Log-Structured Allocator)
- **등급**: MINOR | **스코프**: core | **타입**: design-debt
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: 현 아키텍처(bump+slab+arena)에서 외부 단편화 거의 없어 구조적 불필요. GB급 인메모리 캐시 도입 시만 재평가.

### #H8. new_refresh_token E2E 테스트 미검증
- **등급**: MAJOR | **스코프**: auth-svc | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: 98eca92
- **비고**: e2e_auth_test.cpp에서 token refresh flow + new_refresh_token 필드 검증 완료. 11/11 E2E 통과.

### #H7. Session async_recv 테스트
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_session.cpp에 10+ 시나리오 (정상 read, frame buffering, EOF, 에러). 71/71 유닛 통과.

### #H6. RedisMultiplexer 코루틴 명령 테스트
- **등급**: MAJOR | **스코프**: shared | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_redis_adapter.cpp에서 async 명령 실행 + 에러 처리 검증 완료.

### #H5. ConnectionHandler 단위 테스트 부재
- **등급**: MAJOR | **스코프**: core | **타입**: test
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: test_connection_handler.cpp 추가. 9+ 테스트 (accept, read loop, dispatch, session lifecycle, multi-listener). 71/71 통과.

### #H4. review 문서 2개 상세 내용 부재
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: WONTFIX
- **비고**: v0.5 Wave 1 review 원본 데이터 없이 복원 불가. 초기 레거시.

### #H3. E2E 테스트 실행 가이드 문서
- **등급**: MAJOR | **스코프**: docs, infra | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: apex_services/tests/e2e/CLAUDE.md에 Docker 셋업, 서비스 라이프사이클, 트러블슈팅 6섹션, 테스트 매트릭스 완성.

### #H2. ResponseDispatcher 하드코딩 오프셋
- **등급**: MINOR | **스코프**: gateway | **타입**: bug
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED | **커밋**: df33f60
- **비고**: envelope_payload_offset() 함수 호출로 동적 계산으로 교체.

### #H1. 별도 백로그 파일 2건 미이전
- **등급**: MINOR | **스코프**: docs | **타입**: docs
- **해결**: 2026-03-18 12:53:47 | **방식**: FIXED
- **비고**: backlog_memory_os_level.md + 20260315_094300_backlog.md → BACKLOG.md 통합 완료, 원본 삭제.

---
