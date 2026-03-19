# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 74

---

## NOW

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

### #7. Linux CI 파이프라인 확장 (E2E + UBSAN + Valgrind)
- **등급**: MAJOR
- **스코프**: ci, infra
- **타입**: infra
- **연관**: #9
- **설명**: ① E2E-in-CI docker-compose 기동 + ctest -L e2e ② UBSAN 플래그 누락 교정 ③ Valgrind memcheck job 추가 검토.

### #9. CI에서 Windows apex_shared 어댑터 빌드 미검증
- **등급**: MAJOR
- **스코프**: ci, shared
- **타입**: infra
- **연관**: #7
- **설명**: CI Windows job이 apex_core만 빌드. apex_shared 미커버.

### #73. Docker 로컬 리눅스 빌드 스크립트 + 빌드 락 연동
- **등급**: MAJOR
- **스코프**: tools, infra
- **타입**: infra
- **연관**: #7, #9
- **설명**: 현재 GCC/Clang 경고 확인은 CI push 후에만 가능 (왕복 10-15분). Docker CI 이미지(`ghcr.io/gazuua/apex-pipeline-ci`) 기반 로컬 리눅스 빌드 스크립트 구축: ① `queue-lock.sh`에 `build-linux` 서브커맨드 추가 (빌드 락 연동, MSVC/Docker 동시 빌드 방지) ② MSYS bash 경로 이스케이프 (`//workspace`) 자동 처리 ③ debug/asan/tsan 프리셋 지원 ④ vcpkg 캐시 마운트 (매번 재빌드 방지). GCC 경고 수정 사이클을 1-2분으로 단축.

---

## IN VIEW

### #66. wire_services() co_spawn(detached) → spawn() tracked API 전환
- **등급**: MAJOR
- **스코프**: shared, core
- **타입**: design-debt
- **연관**: #48
- **설명**: `KafkaAdapter::wire_services()`에서 `co_spawn(detached)`를 직접 호출하여 D7 outstanding 코루틴 추적을 우회. shutdown 시 Kafka dispatch 코루틴이 미완료 상태로 CoreEngine이 중단될 수 있음. spawn() tracked API를 사용하도록 변경 필요. wire_services()가 서비스의 spawn()을 호출할 수 있는 경로 설계 필요 (현재 spawn은 protected).

### #67. server.global&lt;T&gt;() / ConsumerPayloadPool / wire_services() 단위 테스트
- **등급**: MAJOR
- **스코프**: core, shared
- **타입**: test
- **연관**: #48
- **설명**: D3/D6/D2 신규 API에 대한 단위 테스트 부재. server.global&lt;T&gt;()의 타입 소거 + 중복 호출, ConsumerPayloadPool의 thread-safe acquire/release + 풀 고갈 fallback, wire_services()의 서비스 자동 감지 등 검증 필요.

### #68. GatewayService set_default_handler() 캡슐화 우회 해소
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: design-debt
- **연관**: #48
- **설명**: GatewayService가 `dispatcher().set_default_handler(lambda)` 형태로 ServiceBase의 set_default_handler(member_function_pointer) 패턴을 우회. 시스템 메시지 분기 로직을 멤버 함수로 이동하면 가이드 §4.1 패턴과 일치.

### #69. 프레임워크 가이드 Shutdown 시퀀스 갱신
- **등급**: MAJOR
- **스코프**: docs
- **타입**: docs
- **연관**: #48
- **설명**: `docs/apex_core/apex_core_guide.md` §3 Shutdown 시퀀스가 실제 `finalize_shutdown()` 구현과 불일치 (Listener stop, Scheduler stop, Adapter drain/close 분리 미반영). 코드 기준으로 갱신 필요.

### #70. 프레임워크 가이드 코드 예시 ServiceRegistry API 수정
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **연관**: #48
- **설명**: 가이드에서 `ctx.local_registry.get<T>()`를 포인터 반환으로 사용하지만 실제 API는 참조 반환 + 미등록 시 예외. `find<T>()` 사용으로 변경 필요.

### #71. PubSubListener mutex 예외 가이드 명시
- **등급**: MINOR
- **스코프**: docs, gateway
- **타ип**: docs
- **연관**: #48
- **설명**: 가이드 §8 #2 "모든 뮤텍스 금지" 원칙에 외부 라이브러리 스레드(hiredis) 예외 조항 명시 필요. PubSubListener는 dedicated Redis 스레드와 서비스 스레드 간 채널 목록 공유에 mutex 사용이 불가피.

### #72. safe_parse_u64 Result&lt;uint64_t&gt; 반환 개선
- **등급**: MINOR
- **스코프**: chat-svc
- **타입**: design-debt
- **연관**: #48
- **설명**: `safe_parse_u64`가 실패 시 0을 반환하는 설계. 0이 유효값인 맥락에서 조용한 실패 가능. `Result<uint64_t>` 반환으로 변경하면 호출부 에러 핸들링 강제 가능.

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
- **연관**: #48
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

### #62. FileWatcher::DetectsChange 간헐 실패 (Windows 타임스탬프 해상도)
- **등급**: MINOR
- **스코프**: gateway
- **타입**: bug
- **설명**: `test_file_watcher.cpp` DetectsChange 테스트가 Windows에서 간헐적으로 실패. 원인: NTFS `last_write_time` 갱신 지연 (100ms~2초). 파일 덮어쓴 직후 `poll_now()` 호출 시 타임스탬프가 아직 갱신되지 않을 수 있음. 수정 방향: `poll_now()` 전 짧은 sleep 추가 또는 파일 내용 해시 기반 비교로 전환.

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

### #63. docs/CLAUDE.md 백로그 운영 규칙 중복 정리
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **설명**: `docs/CLAUDE.md` 백로그 운영 규칙 80줄이 루트 `CLAUDE.md`와 부분 중복. 중복 제거 또는 역할 분리 명확화.

### #64. 서비스 테스트 작성 가이드
- **등급**: MAJOR
- **스코프**: core, docs
- **타입**: docs
- **연관**: #1
- **설명**: 유닛 테스트(GTest + Mock 어댑터) + E2E 테스트 패턴을 다루는 별도 가이드. 프레임워크 가이드(#1)에서 분리된 스코프.

### #65. auto-review 가이드 검증 자동화
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **연관**: #1
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제.

---

## DEFERRED

### #61. 로그 보존 정책 TOML 파라미터화
- **등급**: MINOR
- **스코프**: core
- **타입**: infra
- **설명**: `retention_days` 등으로 자동 삭제 제어. 현재 영구 보존. 디스크 용량 이슈 발생 시 트리거.

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

