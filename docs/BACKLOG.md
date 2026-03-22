# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 132

---

## NOW

### #126. apex-agent: Hook/자동화 시스템 Go 백엔드 재작성
- **등급**: CRITICAL
- **스코프**: tools, infra
- **타입**: infra
- **연관**: #50
- **설명**: 현재 11개 bash 스크립트(~2,080줄)로 구성된 에이전트 hook/자동화 시스템을 Go 단일 바이너리로 재작성. 동기: ① MSYS 경로 버그 반복(#89,#90) ② grep+sed YAML 파싱 fragile ③ 60+ 분기 상태머신의 bash 디버깅 한계 ④ 테스트 불가 ⑤ 스크립트 수 증가 부담. Go 선택 근거: 콜드 스타트 ~10ms(hook 5초 타임아웃 충분), 단일 바이너리(런타임 무의존), `filepath`/`encoding/json`/`gopkg.in/yaml.v3`로 fragile 패턴 해소, 이 도메인(DevOps 오케스트레이션) 표준 언어. 장기적으로 데몬 모드(소켓 IPC, 인메모리 캐시, 이벤트 기반 반응) + SQLite 상태 저장소로 확장하여 코어 프레임워크와 쌍벽을 이루는 에이전트 자동화 백엔드 구축 가능. 대안 언어(Deno/TS 2순위) 및 마이그레이션 전략 포함 상세 논의: `docs/apex_tools/plans/20260322_015226_apex_agent_go_backend.md`

---

## IN VIEW

### #130. TlsTcpTransport::make_socket() static SSL context 멀티코어 unsafe
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: function-local static `ssl::context`가 멀티코어에서 `SSL_CTX` concurrent access 위험. 프로덕션 경로는 `make_socket_with_context()` 사용으로 회피하고 있으나, Transport concept의 `make_socket(io_context&)` 시그니처가 per-core context를 전달할 수 없는 구조적 한계. concept 확장 선행 필요. **[FSD 분석 2026-03-22]** Transport concept 확장이 선행 필요. make_socket(io_context&) 시그니처에 per-core SSL context를 전달할 방법이 없는 구조적 한계. 자동화 불가.

### #131. Kafka 통신 PLAINTEXT — 프로덕션 배포 시 SSL/SASL 필요
- **등급**: MAJOR
- **스코프**: infra
- **타입**: security
- **설명**: Kafka 리스너가 `PLAINTEXT://` 프로토콜만 사용. 개발 환경에서는 적절하지만 프로덕션 배포 시 `SSL` 또는 `SASL_SSL` 필요. KafkaAdapter에 `security.protocol` 설정 메커니즘 추가 필요. **[FSD 분석 2026-03-22]** security.protocol 설정 메커니즘 설계 판단 필요 (KafkaAdapter 설정 확장 방향). 자동화 불가.

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
- **연관**: #127
- **설명**: "direct send + ok()" 패턴의 에러 경로(IP rate limit 거부, JWT 인증 실패, pending map full, route not found)가 미테스트. Mock 의존성이 많아 단위 테스트 인프라 구축 필요. E2E에서 부분 커버. **[FSD 분석 2026-03-22]** 코루틴 + Redis/Kafka/RateLimit mock 인프라 구축이 선행 필요. 테스트 인프라 설계 판단 포함.

### #127. blacklist_fail_open fail-open/fail-close 분기 단위 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **연관**: #102
- **설명**: `gateway_pipeline.cpp`의 `blacklist_fail_open` 설정 기반 fail-open/fail-close 분기 + `BlacklistCheckFailed` 에러 반환 경로가 미테스트. 코루틴 + Redis mock 인프라 필요. #102의 GatewayPipeline 테스트 인프라 구축 시 함께 추가. **[FSD 분석 2026-03-22]** #102 인프라 구축에 의존. 단독 자동화 불가.


### #112. lock-free SessionMap (concurrent_flat_map) 아키텍처 벤치마크
- **등급**: MAJOR
- **스코프**: core, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: Shared 모델에서 SessionMap을 `boost::concurrent_flat_map`으로 교체하여 벤치마킹. io_context 내부 큐가 진짜 병목인지 결정적으로 검증. lock-free SessionMap으로도 처리량이 정체되면 io_context 분리가 유일한 해법임을 증명. **[FSD 분석 2026-03-22]** concurrent_flat_map 교체 + 벤치마크 인프라 구축 필요. 실험적 작업으로 자동화 불가.

### #113. Docker E2E 풀 인프라 벤치마킹
- **등급**: MAJOR
- **스코프**: infra, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: Docker Compose로 Gateway + Auth + Chat + Kafka + Redis + PostgreSQL 전체 인프라를 띄우고 E2E 부하 테스트 실행. 실 서비스 워크로드 기반 처리량/지연시간 측정. 벤치마크 보고서 시스템에 E2E 섹션 추가. **[FSD 분석 2026-03-22]** Docker Compose 전체 인프라 구동 + E2E 부하 테스트 환경 구축 필요. 자동화 불가.

### #114. 프로덕션급 서버 환경 벤치마크 실측
- **등급**: MAJOR
- **스코프**: core, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: 고코어 환경(16코어+ 서버)에서 Per-core vs Shared 아키텍처 비교 재측정. 4코어 노트북에서 관측된 2.1x 차이가 코어 수에 비례하여 확대되는지 검증. 벤치마크 보고서 버전 비교 기능(--baseline/--current) 활용. **[FSD 분석 2026-03-22]** 고코어(16+) 서버 환경 실측 필요. 현재 개발 환경에서 자동화 불가.

### #20. BumpAllocator / ArenaAllocator 벤치마크
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: malloc vs BumpAllocator vs ArenaAllocator 벤치마크 미구현. **[FSD 분석 2026-03-22]** 벤치마크 구현 필요 — 테스트 대상 선정 및 측정 기준에 설계 판단 포함. 자동화 불가.

### #51. Visual Studio + WSL 디버그 환경 구축
- **등급**: MINOR
- **스코프**: infra
- **타입**: infra
- **설명**: IDE 디버그 설정 파일 전무. ① Windows/VS2022 F5 디버깅 타겟 ② WSL/Linux 리모트 디버깅. docker-compose 연동 확인 필수. **[FSD 분석 2026-03-22]** IDE별 환경 설정(launch.json, tasks.json 등) + WSL 리모트 디버깅 구성에 수동 검증 필요. 자동화 불가.

### #104. TSAN suppressions 범위 좁히기
- **등급**: MINOR
- **스코프**: core, infra
- **타입**: infra
- **설명**: `tsan_suppressions.txt`의 `race:boost::asio::detail::*`가 Boost.Asio 전체 내부 레이스를 억제하여 실제 사용 패턴 레이스까지 가릴 수 있음. 구체적 함수명으로 범위를 좁혀야 한다 (예: `race:boost::asio::detail::scheduler::do_run_one`). TSAN 빌드 활성화 후 false positive를 개별 확인하여 정밀 suppression으로 교체. **[FSD 분석 2026-03-22]** TSAN 빌드 실행 후 실제 false positive 리포트 분석이 선행 필수. 자동화 불가.

### #65. auto-review 가이드 검증 자동화
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **연관**: #1 (HISTORY)
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제. **[FSD 분석 2026-03-22]** 옵션 선택 필요 (문자열 기반 diff 스캔 vs AST 파싱). #126 Go 백엔드 재작성과 충돌 가능. 자동화 불가.


---

## DEFERRED

### #21. Server multi-listener dispatcher sync_all_handlers
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 개별 msg_id 핸들러가 primary listener만 적용. 멀티 프로토콜 시 확장 필요. **[FSD 분석 2026-03-22]** 멀티 프로토콜 확장 설계 필요. 자동화 불가.

### #22. async_send_raw + write_pump 동시 write 위험
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 현재 write_pump만 사용하여 미트리거. API 확장 시 동기화 필요. **[FSD 분석 2026-03-22]** write 동기화 메커니즘 설계 필요. API 확장 시점에 트리거. 자동화 불가.

### #61. 로그 보존 정책 TOML 파라미터화
- **등급**: MINOR
- **스코프**: core
- **타입**: infra
- **설명**: `retention_days` 등으로 자동 삭제 제어. 현재 영구 보존. 디스크 용량 이슈 발생 시 트리거. **[FSD 분석 2026-03-22]** TOML 파라미터 스키마 및 자동 삭제 정책 설계 필요. 자동화 불가.

### #50. apex_tools/scripts 폴더 신설 + 스크립트 정리
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **연관**: #126
- **설명**: 독립 실행형 스크립트 3종을 `apex_tools/scripts/`로 이동. 경로 민감 스크립트는 유지.

### #24. 어댑터 상태 관리 불일치
- **등급**: MINOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: KafkaAdapter 자체 AdapterState vs 나머지 AdapterBase::ready_. 정규화 시 통일. **[FSD 분석 2026-03-22]** 통합 방향 선택 필요 — A안: KafkaAdapter의 AdapterState 삭제(ready_ 통일) vs B안: 모든 어댑터에 AdapterState 확장. 자동화 불가.

### #29. drain()/stop() 동일 구현
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: drain=soft close, stop=hard close 분리 검토. **[FSD 분석 2026-03-22]** drain=soft close vs stop=hard close 분리 정책 설계 필요. 자동화 불가.

### #36. Acceptor core 0 부하 불균형
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 단일 acceptor core 0 집중. per-core acceptor 검토. **[FSD 분석 2026-03-22]** per-core acceptor 설계 + 벤치마크 비교 필요. 자동화 불가.

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
