# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 133

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

### #131. Kafka 통신 PLAINTEXT — 프로덕션 배포 시 SSL/SASL 필요
- **등급**: MAJOR
- **스코프**: infra
- **타입**: security
- **설명**: Kafka 리스너가 `PLAINTEXT://` 프로토콜만 사용. 개발 환경에서는 적절하지만 프로덕션 배포 시 `SSL` 또는 `SASL_SSL` 필요. KafkaAdapter에 `security.protocol` 설정 메커니즘 추가 필요. **[FSD 설계 확정 2026-03-22]** B+C 하이브리드 채택: `KafkaSecurityConfig` typed struct(protocol, ssl_ca_location, ssl_cert/key_location, sasl_mechanism, sasl_username/password) + `extra_properties` map(librdkafka passthrough). 업계 표준(Spring Kafka 등) 준용. typed 필드로 IDE 자동완성 + 컴파일 타임 검증 확보, extra_properties로 librdkafka 200+ 설정 커버.

### #130. TlsTcpTransport::make_socket() static SSL context 멀티코어 unsafe
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **설명**: function-local static `ssl::context`가 멀티코어에서 `SSL_CTX` concurrent access 위험. 프로덕션 경로는 `make_socket_with_context()` 사용으로 회피하고 있으나, Transport concept의 `make_socket(io_context&)` 시그니처가 per-core context를 전달할 수 없는 구조적 한계. **[FSD 설계 확정 2026-03-22]** D안 채택 — TransportContext 번들 도입: `struct TransportContext { ssl::context* ssl_ctx = nullptr; /* 향후 metrics*, buffer_pool* 등 확장 */ }`. concept 시그니처를 `make_socket(io_context&, const TransportContext&)`로 1회 변경. 이후 per-core 상태 확장은 struct 필드 추가로 해결. make_socket_with_context() 제거.

### #24. 어댑터 상태 관리 불일치
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **연관**: #29, #132
- **설명**: KafkaAdapter 자체 `AdapterState` enum vs 나머지 `AdapterBase::ready_` bool. 상태 표현이 불일치. **[FSD 설계 확정 2026-03-22]** A안 채택: AdapterBase에 3-state enum `AdapterState { RUNNING, DRAINING, CLOSED }` 도입, 전체 어댑터에 통일. KafkaAdapter의 독자 AdapterState 삭제, `is_ready()` → `state() == RUNNING`으로 전환. #29 drain/stop 분리, #132 async close와 번들 추천.

### #29. drain()/stop() 동일 구현
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **연관**: #24, #132
- **설명**: Listener와 AdapterBase의 `drain()`과 `stop()`이 완전히 동일한 구현. **[FSD 설계 확정 2026-03-22]** A안 채택 — 의미 분리: drain = accept/요청 수신 중단 + in-flight 완료 대기(state → DRAINING), stop = 즉시 종료(state → CLOSED). Listener와 AdapterBase 모두 적용. #24 3-state enum과 연동.

### #132. RedisAdapter::do_close()에서 RedisMultiplexer::close() 미호출
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **연관**: #24, #29, #129 (HISTORY)
- **설명**: `RedisAdapter::do_close()`가 `per_core_.clear()`로 RedisMultiplexer를 동기적으로 파괴하는데, `close()`를 co_await하지 않아 detached 코루틴(reconnect_loop, AUTH)이 파괴된 멤버를 참조할 수 있음. 현재는 shutdown 순서(어댑터 먼저 → io_context drain)에 의해 안전하지만 방어적 보장 부재. **[FSD 설계 확정 2026-03-22]** A안 채택: `AdapterBase::do_close()` 반환 타입을 `awaitable<void>`로 변경. **[FSD 분석 2026-03-22]** A안 구현 시도 중 추가 설계 결정 필요 발견: ① Server::finalize_shutdown()에서 adapter->close()는 core threads 정지 후 호출 — awaitable 실행에 필요한 io_context가 이미 정지됨. ② 종료 순서 변경(adapter close → core stop) 시 기존 안전성 보장 "pending handlers may reference adapter resources" 검증 필요. ③ per-core RedisMultiplexer를 각자의 io_context에서 close하는 cross-io_context 조정 문제. Server shutdown 시퀀스 재설계 후 재시도.

### #19. Auth/Chat 비즈니스 로직 세밀 테스트 부족
- **등급**: MAJOR
- **스코프**: auth-svc, chat-svc
- **타입**: test
- **설명**: 핸들러 디스패치 + msg_id 라우팅 테스트는 구현됨(test_auth_handlers.cpp, test_chat_handlers.cpp). 개별 비즈니스 로직(bcrypt 해싱, 방 인원 제한, 토큰 만료 등)의 세밀한 단위 테스트 커버리지 부족. **[FSD 설계 확정 2026-03-22]** A안 채택: Redis/Kafka 의존 없는 순수 함수로 비즈니스 로직 분리 → 직접 단위 테스트. mock 불필요 방식. 대상: 계정 잠금 카운터 리셋, 토큰 갱신 로직, 방 인원 제한, 메시지 순서 등.

### #102. GatewayPipeline 에러 흐름 단위 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **연관**: #127
- **설명**: "direct send + ok()" 패턴의 에러 경로(IP rate limit 거부, JWT 인증 실패, pending map full, route not found)가 미테스트. **[FSD 설계 확정 2026-03-22]** A안 채택: interface mock + io_context 코루틴 테스트 하네스. RateLimitFacade, JwtBlacklist 등 interface 추출 → mock 구현, `io_context.run()` 기반 코루틴 실행. #127과 번들 필수.

### #127. blacklist_fail_open fail-open/fail-close 분기 단위 테스트
- **등급**: MAJOR
- **스코프**: gateway
- **타입**: test
- **연관**: #102
- **설명**: `gateway_pipeline.cpp`의 `blacklist_fail_open` 설정 기반 fail-open/fail-close 분기 + `BlacklistCheckFailed` 에러 반환 경로가 미테스트. **[FSD 설계 확정 2026-03-22]** A안 채택: #102와 동일 인프라(interface mock + io_context 코루틴 테스트 하네스) 사용. #102와 번들 필수.

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
- **연관**: #1 (HISTORY), #126
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제. **[FSD 설계 확정 2026-03-22]** C안 채택: #126 Go 백엔드 재작성 시 리뷰 검증 기능 통합. #126 완료 후 착수.


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

### #59. 문서 자동화 — 생성 스크립트 + pre-commit 검증 + 템플릿
- **등급**: MAJOR
- **스코프**: tools, docs
- **타입**: infra
- **설명**: 에이전트가 문서 규칙을 무시하는 문제를 규칙이 아닌 코드로 강제. 5가지 자동화: ① `new-doc.sh` — category/project/version/topic 인자, `date` 기반 타임스탬프, 시스템 시간 sanity check, etc 경로 WARNING + 머지 전 보고. ② superpowers 파일 차단 — `.gitignore` + pre-commit hook 워킹 디렉토리 스캔, 커밋 실패 + 재작성 안내. ③ 빈 문서 차단 — pre-commit hook N줄 미만 reject. ④ 타임스탬프 사후 보정 — `fix-doc-timestamps.sh` git log 대조 + 신규 파일 현재 시각 비교. ⑤ 카테고리별 `.template.md`. **후순위 강등(2026-03-22)**: 현재 스크립트 후킹으로 문서 규칙 준수율 충분. #126 Go 백엔드 안정화 후 재평가.

### #50. apex_tools/scripts 폴더 신설 + 스크립트 정리
- **등급**: MINOR
- **스코프**: tools
- **타입**: infra
- **연관**: #126
- **설명**: 독립 실행형 스크립트 3종을 `apex_tools/scripts/`로 이동. 경로 민감 스크립트는 유지. **[FSD 분석 2026-03-22]** #126 Go 백엔드 재작성 진행 중이며 스크립트 구조에 직접 영향. 이동 대상 선별에 판단 필요. #126 완료 후 재평가.

### #36. Acceptor core 0 부하 불균형
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 단일 acceptor core 0 집중. per-core acceptor 검토. **[FSD 분석 2026-03-22]** per-core acceptor 설계 + 벤치마크 비교 필요. 자동화 불가.

### #40. NUMA 바인딩 + Core Affinity
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v1.0+ 멀티 소켓 배포 시 재평가. **[FSD 분석 2026-03-22]** v1.0+ 전제 조건 미충족. 트리거 대기.

### #41. mmap 직접 사용 (malloc 대체)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v0.6 RSS 모니터링 도입 시 재평가. **[FSD 분석 2026-03-22]** v0.6 RSS 모니터링 전제 조건 미충족. 트리거 대기.

### #42. Hugepage (대형 페이지)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 부하 테스트에서 TLB miss 병목 확인 후. **[FSD 분석 2026-03-22]** TLB miss 병목 확인 전제 조건 미충족. 트리거 대기.

### #43. L1 로컬 캐시
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: v1.0+ 이후. **[FSD 분석 2026-03-22]** v1.0+ 전제 조건 미충족. 트리거 대기.

### #44. 코루틴 프레임 풀 할당 (ADR-21)
- **등급**: MINOR
- **스코프**: core
- **타입**: perf
- **설명**: 벤치마크에서 병목 확인 시 도입. **[FSD 분석 2026-03-22]** 벤치마크 병목 확인 전제 조건 미충족. 트리거 대기.
