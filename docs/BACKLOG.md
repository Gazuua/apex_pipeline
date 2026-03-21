# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 126

---

## NOW

(없음)

---

## IN VIEW

### #59. 문서 자동화 — 생성 스크립트 + pre-commit 검증 + 템플릿
- **등급**: MAJOR
- **스코프**: tools, docs
- **타입**: infra
- **설명**: 에이전트가 문서 규칙을 무시하는 문제를 규칙이 아닌 코드로 강제. 5가지 자동화: ① `new-doc.sh` — category/project/version/topic 인자, `date` 기반 타임스탬프, 시스템 시간 sanity check, etc 경로 WARNING + 머지 전 보고. ② superpowers 파일 차단 — `.gitignore` + pre-commit hook 워킹 디렉토리 스캔, 커밋 실패 + 재작성 안내. ③ 빈 문서 차단 — pre-commit hook N줄 미만 reject. ④ 타임스탬프 사후 보정 — `fix-doc-timestamps.sh` git log 대조 + 신규 파일 현재 시각 비교. ⑤ 카테고리별 `.template.md`. **[FSD 분석 2026-03-22]** 5종 도구 각각 동작 정의·threshold·예외 처리에 설계 판단 필요. 기계적 자동화 불가.

### #19. Auth/Chat 비즈니스 로직 세밀 테스트 부족
- **등급**: MAJOR
- **스코프**: auth-svc, chat-svc
- **타입**: test
- **설명**: 핸들러 디스패치 + msg_id 라우팅 테스트는 구현됨(test_auth_handlers.cpp, test_chat_handlers.cpp). 개별 비즈니스 로직(bcrypt 해싱, 방 인원 제한, 토큰 만료 등)의 세밀한 단위 테스트 커버리지 부족. **[FSD 분석 2026-03-22]** 테스트 대상 비즈니스 로직 선정, mock 전략, 커버리지 목표에 설계 판단 필요.

### #102. GatewayPipeline 에러 흐름 단위 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **연관**: #123
- **설명**: "direct send + ok()" 패턴의 에러 경로(IP rate limit 거부, JWT 인증 실패, pending map full, route not found)가 미테스트. Mock 의존성이 많아 단위 테스트 인프라 구축 필요. E2E에서 부분 커버. **[FSD 분석 2026-03-22]** 코루틴 + Redis/Kafka/RateLimit mock 인프라 구축이 선행 필요. 테스트 인프라 설계 판단 포함.

### #123. blacklist_fail_open fail-open/fail-close 분기 단위 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **연관**: #102
- **설명**: `gateway_pipeline.cpp`의 `blacklist_fail_open` 설정 기반 fail-open/fail-close 분기 + `BlacklistCheckFailed` 에러 반환 경로가 미테스트. 코루틴 + Redis mock 인프라 필요. #102의 GatewayPipeline 테스트 인프라 구축 시 함께 추가. **[FSD 분석 2026-03-22]** #102 인프라 구축에 의존. 단독 자동화 불가.

### #124. jwt_blacklist is_valid_jti 입력 검증 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **설명**: `jwt_blacklist.cpp`의 `is_valid_jti` 함수(빈 문자열, 128자 초과, 허용 문자 외 입력)가 미테스트. JTI Redis injection 방어 로직의 정확성 검증 필요.

### #125. init.sql auth_schema 고아 스키마 정리
- **등급**: MINOR
- **스코프**: infra
- **타입**: design-debt
- **설명**: `apex_infra/postgres/init.sql`에서 `CREATE SCHEMA IF NOT EXISTS auth_schema`를 생성하지만, `001_create_schema_and_role.sql`은 `auth_svc` 스키마를 사용. `auth_schema`는 아무도 참조하지 않는 고아 스키마. 정리 또는 통일 필요.

### #49. Docker 이미지 버전 감사 + pgbouncer 교체
- **등급**: MAJOR
- **스코프**: infra
- **타입**: infra
- **설명**: `edoburu/pgbouncer:1.23.1` pull 실패 → `bitnami/pgbouncer` 교체. redis/postgres 마이너 핀닝 검토. dev + e2e 양쪽 compose 갱신.

### #122. CI Docker :latest → immutable 태그 전환
- **등급**: MAJOR
- **스코프**: infra
- **타입**: infra
- **연관**: #49
- **설명**: CI 빌드 잡에서 `ghcr.io/gazuua/apex-pipeline-ci:latest`를 사용하여 빌드 간 재현성 미보장. sha 태그 또는 digest 기반 pinning으로 전환. Docker 이미지 빌드 시 이미 `sha-${{ github.sha }}` 태그를 생성하고 있으므로 참조만 변경하면 됨. **[FSD 분석 2026-03-22]** :latest 제거 시 build cache-from 전략 재설계 필요 (현재 :latest를 캐시 소스로 사용). 단순 참조 변경이 아님.

### #121. ci.Dockerfile non-root 실행 + .dockerignore 서비스 빌드 호환
- **등급**: MAJOR
- **스코프**: infra
- **타입**: infra
- **설명**: ① ci.Dockerfile이 `USER` 지시자 없이 root로 빌드 실행. 비특권 사용자 추가 필요. ② `.dockerignore`가 `*` + whitelist(vcpkg.json만) 방식이라 서비스 Dockerfile의 `COPY . /src`에서 소스 파일이 복사되지 않을 가능성. 서비스 빌드 컨텍스트 검증 후 whitelist 확장 필요.
### #112. lock-free SessionMap (concurrent_flat_map) 아키텍처 벤치마크
- **등급**: MAJOR
- **스코프**: core, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: Shared 모델에서 SessionMap을 `boost::concurrent_flat_map`으로 교체하여 벤치마킹. io_context 내부 큐가 진짜 병목인지 결정적으로 검증. lock-free SessionMap으로도 처리량이 정체되면 io_context 분리가 유일한 해법임을 증명.

### #113. Docker E2E 풀 인프라 벤치마킹
- **등급**: MAJOR
- **스코프**: infra, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: Docker Compose로 Gateway + Auth + Chat + Kafka + Redis + PostgreSQL 전체 인프라를 띄우고 E2E 부하 테스트 실행. 실 서비스 워크로드 기반 처리량/지연시간 측정. 벤치마크 보고서 시스템에 E2E 섹션 추가.

### #114. 프로덕션급 서버 환경 벤치마크 실측
- **등급**: MAJOR
- **스코프**: core, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: 고코어 환경(16코어+ 서버)에서 Per-core vs Shared 아키텍처 비교 재측정. 4코어 노트북에서 관측된 2.1x 차이가 코어 수에 비례하여 확대되는지 검증. 벤치마크 보고서 버전 비교 기능(--baseline/--current) 활용.

### #20. BumpAllocator / ArenaAllocator 벤치마크
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: malloc vs BumpAllocator vs ArenaAllocator 벤치마크 미구현.

### #51. Visual Studio + WSL 디버그 환경 구축
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: IDE 디버그 설정 파일 전무. ① Windows/VS2022 F5 디버깅 타겟 ② WSL/Linux 리모트 디버깅. docker-compose 연동 확인 필수.

### #104. TSAN suppressions 범위 좁히기
- **등급**: MINOR
- **스코프**: core, infra
- **타입**: infra
- **설명**: `tsan_suppressions.txt`의 `race:boost::asio::detail::*`가 Boost.Asio 전체 내부 레이스를 억제하여 실제 사용 패턴 레이스까지 가릴 수 있음. 구체적 함수명으로 범위를 좁혀야 한다 (예: `race:boost::asio::detail::scheduler::do_run_one`). TSAN 빌드 활성화 후 false positive를 개별 확인하여 정밀 suppression으로 교체.

### #65. auto-review 가이드 검증 자동화
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **연관**: #1
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제.

### #63. docs/CLAUDE.md 백로그 운영 규칙 중복 정리
- **등급**: MINOR
- **스코프**: docs
- **타입**: docs
- **설명**: `docs/CLAUDE.md` 백로그 운영 규칙 80줄이 루트 `CLAUDE.md`와 부분 중복. 중복 제거 또는 역할 분리 명확화.

---

## DEFERRED

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
