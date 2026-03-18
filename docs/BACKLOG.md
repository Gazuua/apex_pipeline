# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 61

---

## NOW


### #48. Post-E2E 코드 리뷰 (10개 관점)
- **등급**: CRITICAL
- **스코프**: core, gateway, auth-svc, chat-svc, shared
- **타입**: design-debt
- **연관**: #52
- **설명**: v0.5.5.1 E2E 11/11 통과 후 코드 품질 리뷰 미수행 상태. PR #27~#37 (101파일, ~7,863 insertions) 대상으로 10개 관점 체계 리뷰 수행. 관점: ① 코어 인터페이스 단순화 ② 초기화 순서 의존성 ③ OOP/유지보수성 ④ shared-nothing 준수 ⑤ 서비스 간 의존성 ⑥ 코루틴 lifetime ⑦ 에러 핸들링 일관성 ⑧ shutdown 경로 정합성 ⑨ 매직넘버/하드코딩 ⑩ 에이전트 자율 판단. 상세 계획: `docs/apex_common/plans/20260318_144700_post-e2e-code-review.md` 참조. v0.6 진입 전 반드시 완료.

### #1. apex_core 프레임워크 가이드 (API 가이드 + 내부 아키텍처)
- **등급**: CRITICAL
- **스코프**: core, docs
- **타입**: docs
- **연관**: #56
- **설명**: 서비스 개발자와 에이전트 양쪽이 이 문서만 보고 새 서비스를 프레임워크 위에 올릴 수 있도록 하는 통합 가이드. 현재는 Gateway/Auth/Chat 구현 코드를 직접 역추적해야 하며 반복 시행착오 발생. **2레이어 문서 구조**: **레이어 1 — 서비스 개발 API 가이드** (내부를 몰라도 서비스를 만들 수 있는 인터페이스 레퍼런스): ServiceBase<T> 상속 및 라이프사이클 훅(on_configure/on_wire/on_start/on_stop/on_session_closed), 핸들러 등록 3종(handle/route<T>/kafka_route<T>), set_default_handler(프록시 패턴), ConfigureContext·WireContext 사용 가능 API, bump()/arena() 사용법, 하지 말아야 할 것 목록(코어 간 직접 공유, Phase 순서 위반, co_await 후 FlatBuffers 포인터 접근 등). **레이어 2 — 프레임워크 내부 아키텍처** (왜 이렇게 동작하는지): 아키텍처 구조도(Server/CoreEngine/Listener/ConnectionHandler per-core 배치), 클래스 다이어그램 + 주요 클래스 호출 관계, Phase 1→2→3→3.5 시퀀스, AdapterBase 초기화 순서, ResponseDispatcher 배선, standalone CoreEngine 패턴. 2단계 전략: 1차 md 초안(에이전트 친화 구조 — 계층별 요약→상세 점진적 깊이), 2차 코어 안정화 후 다이어그램 그림 승격. v0.6 서비스 온보딩의 선행 조건.

### #60. 로그 디렉토리 구조 확립 + 경로 중앙화 + 파일명 표준화
- **등급**: MAJOR
- **스코프**: core, gateway, auth-svc, chat-svc, infra
- **타입**: infra
- **연관**: #52
- **설명**: 로그 파일이 프로젝트 루트에 산란하는 문제. 3가지 작업: ① **루트 로그 디렉토리 지정** — TOML `logging.file.path`를 서비스별/환경별 구조화된 디렉토리로 변경 (예: `logs/{service}/{env}/`). ② **로그 디렉토리 구조 결정** — 서비스명/날짜/환경 기반 계층 설계. ③ **로그 파일명 형식 표준화** — `{service}_{date}_{pid}.log` 등 구조화된 네이밍. E2E fixture(`e2e_test_fixture.cpp:50`)의 하드코딩 `name + "_e2e.log"` → 루트 산란도 이 항목에서 해결. `.gitignore`에 `*.log` 포함되어 git 오염은 없으나 작업 디렉토리 위생 문제.

---

## IN VIEW

### #2. RedisMultiplexer cancel_all_pending UAF
- **등급**: CRITICAL
- **스코프**: shared
- **타입**: bug
- **설명**: `cancel_all_pending()`에서 timed_out 경로와 async_wait 경로 간 UAF 가능. `redis_multiplexer.cpp` 361-366 주석 참조.

### #3. Protocol concept Frame 내부 구조 미제약
- **등급**: CRITICAL
- **스코프**: core
- **타입**: design-debt
- **설명**: Protocol concept이 Frame 내부 구조를 명시적으로 요구하지 않음. accessor 메서드 요구 또는 Frame trait 도입 필요.

### #58. 코딩 컨벤션 확립 + .clang-format 도입 + 전체 일괄 포맷팅
- **등급**: MAJOR
- **스코프**: core, shared, gateway, auth-svc, chat-svc, ci
- **타입**: infra
- **연관**: #54
- **설명**: 코딩 스타일이 K&R/Allman 혼용 상태(K&R 90%, Allman은 생성자 이니셜라이저만). 멤버 변수 trailing underscore, snake_case 네이밍은 이미 일관적. 3단계 작업: ① `.clang-format` 설정 파일 작성 — Allman brace(`BreakBeforeBraces: Allman`), 인덴트, 줄 길이, 네이밍 등 프로젝트 컨벤션 확정. 구현 시 세부 옵션 브레인스토밍 ② 전체 코드베이스 `clang-format -i` 일괄 포맷팅 + `.git-blame-ignore-revs`에 포맷팅 커밋 등록(blame 히스토리 보존) ③ CI에 `clang-format --dry-run --Werror` 추가로 이후 스타일 위반 자동 차단. v0.6에서 코드량 증가 전에 적용해야 변환 비용 최소.

### #54. 빌드/정적분석 경고 전수 소탕 + 경고 레벨 확립
- **등급**: MAJOR
- **스코프**: core, shared, gateway, auth-svc, chat-svc, ci
- **타입**: infra
- **연관**: #58
- **설명**: CMakeLists.txt에 `-Wall`/`-Wextra`(GCC/Clang), `/W4`(MSVC) 플래그 자체가 미설정 — 컴파일러 기본 경고만 출력 중. 3단계 작업: ① CMake에 경고 플래그 추가 + 외부 라이브러리 헤더는 system include로 경고 억제 ② 전체 빌드 후 발생하는 GCC/Clang/MSVC 경고 전수 수정 또는 의도적 억제 ③ clangd 진단 경고 잔여분 소탕 (session.cpp 3건 포함, 기존 #30 흡수) ④ CI에서 `-Werror`/`/WX` 격상 여부 판단. 구현 시 경고 건수 측정 후 수정 범위 브레인스토밍.

### #9. CI에서 Windows apex_shared 어댑터 빌드 미검증
- **등급**: MAJOR
- **스코프**: ci, shared
- **타입**: infra
- **설명**: CI Windows job이 apex_core만 빌드. apex_shared 미커버.

### #7. Linux CI 파이프라인 확장 (E2E + UBSAN + Valgrind)
- **등급**: MAJOR
- **스코프**: ci, infra
- **타입**: infra
- **설명**: ① E2E-in-CI docker-compose 기동 + ctest -L e2e ② UBSAN 플래그 누락 교정 ③ Valgrind memcheck job 추가 검토.

### #5. gateway.toml 시크릿 운영 환경 관리
- **등급**: MAJOR
- **스코프**: infra, gateway
- **타입**: security
- **연관**: #6, #8
- **설명**: Redis/PgBouncer 프로덕션 시크릿 주입 전략 (Docker Secrets 또는 K8s ConfigMap).

### #6. SQL 마이그레이션 DB 역할 비밀번호 하드코딩
- **등급**: MAJOR
- **스코프**: infra, auth-svc
- **타입**: security
- **연관**: #5, #8
- **설명**: 평문 비밀번호 하드코딩. 환경 변수 치환 미구현.

### #8. Redis 4인스턴스 무인증 + PgBouncer 평문 비밀번호
- **등급**: MAJOR
- **스코프**: infra
- **타입**: security
- **연관**: #5, #6
- **설명**: 프로덕션 v0.6 배포 전 Redis ACL + PgBouncer SCRAM-SHA-256 + 시크릿 주입 필수.

### #49. Docker 이미지 버전 감사 + pgbouncer 교체
- **등급**: MAJOR
- **스코프**: infra
- **타입**: infra
- **설명**: `edoburu/pgbouncer:1.23.1` pull 실패 → `bitnami/pgbouncer` 교체. redis/postgres 마이너 핀닝 검토. dev + e2e 양쪽 compose 갱신.

### #52. 디버깅/운영 흐름 로깅 대폭 추가
- **등급**: MAJOR
- **스코프**: core, shared, gateway, auth-svc, chat-svc
- **타입**: infra
- **연관**: #48, #60
- **설명**: 현재 spdlog 호출 249건 중 debug 10건, trace 0건. 코어 핫패스 10개 소스에 로깅 전무. 개선: ① 코어 핫패스 debug/trace 추가 ② named logger 전환 ③ core_id/session_id 체계 포함 ④ MDC trace_id 활성화. #48 코드 리뷰 결과에 따라 범위 조정.

### #4. Assertion 크래시 시 __FUNCTION__ / __LINE__ 로깅
- **등급**: MAJOR
- **스코프**: core, infra
- **타입**: infra
- **설명**: assertion 실패 시 위치 정보 없이 크래시. 시그널 핸들러 로깅 필요.

### #56. 서비스 레이어 가드레일 — 코어 인터페이스 캡슐화 + 원칙 위반 방지
- **등급**: MAJOR
- **스코프**: core, shared
- **타입**: design-debt
- **연관**: #1
- **설명**: 서비스 코드가 shared-nothing 원칙을 깨거나 프레임워크 우회 코드를 작성하는 것을 설계 레벨에서 방지. 방향: ① io_context 직접 접근 차단 — 캡슐화 인터페이스 제공 ② 인터페이스 갭 → 코어 확장 ③ 힙 할당은 가이드(금지 아님). #1과 연계.

### #59. 문서 자동화 — 생성 스크립트 + pre-commit 검증 + 템플릿
- **등급**: MAJOR
- **스코프**: tools, docs
- **타입**: infra
- **설명**: 에이전트가 문서 규칙을 무시하는 문제를 규칙이 아닌 코드로 강제. 5가지 자동화: ① `new-doc.sh` — category/project/version/topic 인자, `date` 기반 타임스탬프, 시스템 시간 sanity check, etc 경로 WARNING + 머지 전 보고. ② superpowers 파일 차단 — `.gitignore` + pre-commit hook 워킹 디렉토리 스캔, 커밋 실패 + 재작성 안내. ③ 빈 문서 차단 — pre-commit hook N줄 미만 reject. ④ 타임스탬프 사후 보정 — `fix-doc-timestamps.sh` git log 대조 + 신규 파일 현재 시각 비교. ⑤ 카테고리별 `.template.md`.

### #10. CircuitBreaker HALF_OPEN 코루틴 인터리빙
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **연관**: #11
- **설명**: should_allow() 비원자적. 어댑터 통합 시점에 수정.

### #11. CircuitBreaker::call() Result<void> 타입 제한
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **연관**: #10
- **설명**: Result<T> 제네릭 확장 필요.

### #13. Listener<P> 단위 테스트 부재
- **등급**: MAJOR
- **스코프**: core
- **타입**: test
- **설명**: start/drain/stop, per-core handler 동기화, acceptor 관리 단위 테스트 없음.

### #14. WebSocketProtocol 테스트 부재
- **등급**: MAJOR
- **스코프**: shared
- **타입**: test
- **설명**: try_decode(), consume_frame() 단위 테스트 미작성.

### #16. PgTransaction begun_ 경로 unit test
- **등급**: MAJOR
- **스코프**: shared
- **타입**: test
- **설명**: MockPgConnection 필요.

### #19. Auth/Chat 비즈니스 로직 세밀 테스트 부족
- **등급**: MAJOR
- **스코프**: auth-svc, chat-svc
- **타입**: test
- **설명**: 핸들러 디스패치 + msg_id 라우팅 테스트는 구현됨(test_auth_handlers.cpp, test_chat_handlers.cpp). 개별 비즈니스 로직(bcrypt 해싱, 방 인원 제한, 토큰 만료 등)의 세밀한 단위 테스트 커버리지 부족.

### #20. BumpAllocator / ArenaAllocator 벤치마크
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: malloc vs BumpAllocator vs ArenaAllocator 벤치마크 미구현.

### #47. README 리뉴얼 (빌드 안내 + 프로젝트 소개 + 퀵스타트)
- **등급**: MAJOR
- **스코프**: docs
- **타입**: docs
- **설명**: 공개 임박 시점에서 프로젝트 첫인상 역할. 프로젝트 소개, 아키텍처 개요, 퀵스타트, 빌드 가이드 포함 리뉴얼. README는 진입점 + 링크 허브 역할.

### #51. Visual Studio + WSL 디버그 환경 구축
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: IDE 디버그 설정 파일 전무. ① Windows/VS2022 F5 디버깅 타겟 ② WSL/Linux 리모트 디버깅. docker-compose 연동 확인 필수.

---

## DEFERRED

### #50. apex_tools/scripts 폴더 신설 + 스크립트 정리
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **설명**: 독립 실행형 스크립트 3종을 `apex_tools/scripts/`로 이동. 경로 민감 스크립트는 유지.

### #21. Server multi-listener dispatcher sync_all_handlers
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 개별 msg_id 핸들러가 primary listener만 적용. 멀티 프로토콜 시 확장 필요.

### #22. async_send_raw + write_pump 동시 write 위험
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 현재 write_pump만 사용하여 미트리거. API 확장 시 동기화 필요.

### #23. TOCTOU: join_room SCARD→SADD 경합
- **등급**: MAJOR
- **스코프**: chat-svc
- **타입**: bug
- **설명**: Redis Lua script 원자적 처리 필요하나 어댑터 Lua 지원 미구현. 발생 빈도 극히 낮음.

### #24. 어댑터 상태 관리 불일치
- **등급**: MINOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: KafkaAdapter 자체 AdapterState vs 나머지 AdapterBase::ready_. 정규화 시 통일.

### #26. ReplyTopicHeader::serialize() silent failure
- **등급**: MINOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: overflow 시 빈 vector 반환. `std::expected` 통일 검토.

### #27. FrameError→ErrorCode 매핑 구분 불가
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 모두 ErrorCode::InvalidMessage로 매핑. 세분화 검토.

### #29. drain()/stop() 동일 구현
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: drain=soft close, stop=hard close 분리 검토.

### #36. Acceptor core 0 부하 불균형
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 단일 acceptor core 0 집중. per-core acceptor 검토.

### #38. boost-beast 조기 추가
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: Beast 통합 전까지 미사용 의존성.

### #39. CMakeLists.txt 하드코딩 상대 경로
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: CMake 변수 활용 검토.

### #40. NUMA 바인딩 + Core Affinity
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v1.0+ 멀티 소켓 배포 시 재평가.

### #41. mmap 직접 사용 (malloc 대체)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v0.6 RSS 모니터링 도입 시 재평가.

### #42. Hugepage (대형 페이지)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 부하 테스트에서 TLB miss 병목 확인 후.

### #43. L1 로컬 캐시
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v1.0+ 이후.

### #44. 코루틴 프레임 풀 할당 (ADR-21)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 벤치마크에서 병목 확인 시 도입.

