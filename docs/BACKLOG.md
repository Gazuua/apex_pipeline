# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 136

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

### #132. RedisAdapter::do_close()에서 RedisMultiplexer::close() 미호출
- **등급**: MAJOR
- **스코프**: shared
- **타입**: design-debt
- **연관**: #24, #29, #129 (HISTORY)
- **설명**: `RedisAdapter::do_close()`가 `per_core_.clear()`로 RedisMultiplexer를 동기적으로 파괴하는데, `close()`를 co_await하지 않아 detached 코루틴(reconnect_loop, AUTH)이 파괴된 멤버를 참조할 수 있음. 현재는 shutdown 순서(어댑터 먼저 → io_context drain)에 의해 안전하지만 방어적 보장 부재. **[FSD 설계 확정 2026-03-22]** ~~A안: do_close() → awaitable~~ → ~~B안: shutdown 시퀀스 재배치~~ → **[FSD 분석 2026-03-22]** B안 구현 시도 중 UAF 위험 발견: 단순 재배치(close → stop)하면 io_context가 살아있는 동안 detached 코루틴(reconnect_loop, AUTH)이 파괴된 multiplexer 멤버에 접근 가능. 현재 순서(stop → close)가 오히려 안전 — io_context 정지 후 코루틴이 재개 없이 파괴됨(Boost.Asio ~io_context). **올바른 해결 방향**: ① do_drain()에서 detached 코루틴을 cancellation_signal로 명시적 취소 ② outstanding 카운터에 어댑터 내부 코루틴도 포함 ③ drain 완료 확인 후 재배치 가능. cancellation 인프라 구축이 선행 필요.

### #133. TransportContext의 ssl::context* — apex_core에 OpenSSL 직접 의존
- **등급**: MAJOR
- **스코프**: core, shared
- **타입**: design-debt
- **연관**: #130 (이번 PR에서 도입)
- **설명**: `TransportContext`가 `boost::asio::ssl::context*`를 직접 보유하여 apex_core에 OpenSSL 의존이 발생. **[FSD 설계 확정 2026-03-22]** B안 채택: Virtual SocketBase wrapper. Session이 `unique_ptr<SocketBase>` 보유, TcpSocket/TlsSocket 구현체. ssl::context는 Listener<P, TlsTcpTransport>가 소유. Session/SessionManager 비템플릿 유지. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

### #135. KafkaSecurityConfig 시크릿 처리 — sasl_password 평문 저장
- **등급**: MINOR
- **스코프**: shared
- **타입**: security
- **연관**: #131 (이번 PR에서 도입)
- **설명**: `KafkaSecurityConfig::sasl_password`가 `std::string` 평문 저장. 로그 출력은 마스킹 처리됐으나, 메모리 상 평문 잔존. v0.6 운영 인프라 마일스톤에서 secure secret 처리 방식(환경 변수 참조, 암호화 등) 결정. **[FSD 분석 2026-03-22]** v0.6 운영 인프라 마일스톤에서 시크릿 관리 방식 결정 후 착수. 현재 자동화 불가.

### #112. lock-free SessionMap (concurrent_flat_map) 아키텍처 벤치마크
- **등급**: MAJOR
- **스코프**: core, tools
- **타입**: perf
- **연관**: #107 (HISTORY)
- **설명**: Shared 모델에서 SessionMap을 `boost::concurrent_flat_map`으로 교체하여 벤치마킹. io_context 내부 큐가 진짜 병목인지 결정적으로 검증. **[FSD 설계 확정 2026-03-22]** A안 채택: 기존 `bench_architecture_comparison.cpp`에 `BM_Shared_LockFree_Stateful` 변형 추가. Per-core vs sharded_mutex vs concurrent_flat_map 3자 비교. Boost 1.84.0+ 이미 가용. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

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
- **설명**: malloc vs BumpAllocator vs ArenaAllocator 벤치마크. **[FSD 설계 확정 2026-03-22]** 기존 micro 유지 + RequestCycle(할당 3~8회 가변 32~512B → reset) + TransactionCycle(블록 경계 넘는 가변 할당 → reset) 추가. Capacity 파라미터 스윕: Bump {16K,64K,256K}, Arena block {1K,4K,16K}. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

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
- **설명**: 개별 msg_id 핸들러가 primary listener만 적용. 멀티 프로토콜 시 확장 필요. **[FSD 설계 확정 2026-03-22]** A안 채택: `ListenerBase::sync_all_handlers()` + `MessageDispatcher::handlers() const` 접근자 추가. Phase 3.5에서 전체 핸들러 맵 복사. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

### #22. async_send_raw + write_pump 동시 write 위험
- **등급**: MAJOR
- **스코프**: core
- **타입**: design-debt
- **설명**: `async_send_raw`가 소켓에 직접 `async_write` 호출 — `write_pump`와 동시 실행 시 UB. **[FSD 설계 확정 2026-03-22]** B안 채택: `async_send_raw` 시그니처 유지, 내부적으로 `enqueue_write` + completion promise 패턴. write_pump가 항목 처리 후 promise 이행. `async_send`도 동일 적용. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

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
