# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 150

---

## NOW

---

## IN VIEW

### #133. TransportContext의 ssl::context* — apex_core에 OpenSSL 직접 의존
- **등급**: MAJOR
- **스코프**: CORE, SHARED
- **타입**: DESIGN_DEBT
- **연관**: #130(HISTORY)
- **설명**: `TransportContext`가 `boost::asio::ssl::context*`를 직접 보유하여 apex_core에 OpenSSL 의존이 발생. **[FSD 설계 확정 2026-03-22]** B안 채택: Virtual SocketBase wrapper. Session이 `unique_ptr<SocketBase>` 보유, TcpSocket/TlsSocket 구현체. ssl::context는 Listener<P, TlsTcpTransport>가 소유. Session/SessionManager 비템플릿 유지. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md`

### #135. KafkaSecurityConfig 시크릿 처리 — sasl_password 평문 저장
- **등급**: MINOR
- **스코프**: SHARED
- **타입**: SECURITY
- **연관**: #131(HISTORY)
- **설명**: `KafkaSecurityConfig::sasl_password`가 `std::string` 평문 저장. 로그 출력은 마스킹 처리됐으나, 메모리 상 평문 잔존. v0.6 운영 인프라 마일스톤에서 secure secret 처리 방식(환경 변수 참조, 암호화 등) 결정. **[FSD 분석 2026-03-22]** v0.6 운영 인프라 마일스톤에서 시크릿 관리 방식 결정 후 착수. 현재 자동화 불가.

### #113. Docker E2E 풀 인프라 벤치마킹
- **등급**: MAJOR
- **스코프**: INFRA, TOOLS
- **타입**: PERF
- **연관**: #107(HISTORY)
- **설명**: Docker Compose로 Gateway + Auth + Chat + Kafka + Redis + PostgreSQL 전체 인프라를 띄우고 E2E 부하 테스트 실행. 실 서비스 워크로드 기반 처리량/지연시간 측정. 벤치마크 보고서 시스템에 E2E 섹션 추가. **[FSD 분석 2026-03-22]** Docker Compose 전체 인프라 구동 + E2E 부하 테스트 환경 구축 필요. 자동화 불가.

### #114. 프로덕션급 서버 환경 벤치마크 실측
- **등급**: MAJOR
- **스코프**: CORE, TOOLS
- **타입**: PERF
- **연관**: #107(HISTORY)
- **설명**: 고코어 환경(16코어+ 서버)에서 Per-core vs Shared 아키텍처 비교 재측정. 4코어 노트북에서 관측된 2.1x 차이가 코어 수에 비례하여 확대되는지 검증. 벤치마크 보고서 버전 비교 기능(--baseline/--current) 활용. **[FSD 분석 2026-03-22]** 고코어(16+) 서버 환경 실측 필요. 현재 개발 환경에서 자동화 불가.

### #51. Visual Studio + WSL 디버그 환경 구축
- **등급**: MINOR
- **스코프**: INFRA
- **타입**: INFRA
- **설명**: IDE 디버그 설정 파일 전무. ① Windows/VS2022 F5 디버깅 타겟 ② WSL/Linux 리모트 디버깅. docker-compose 연동 확인 필수. **[FSD 분석 2026-03-22]** IDE별 환경 설정(launch.json, tasks.json 등) + WSL 리모트 디버깅 구성에 수동 검증 필요. 자동화 불가.

### #104. TSAN suppressions 범위 좁히기
- **등급**: MINOR
- **스코프**: CORE, INFRA
- **타입**: INFRA
- **설명**: `tsan_suppressions.txt`의 `race:boost::asio::detail::*`가 Boost.Asio 전체 내부 레이스를 억제하여 실제 사용 패턴 레이스까지 가릴 수 있음. 구체적 함수명으로 범위를 좁혀야 한다 (예: `race:boost::asio::detail::scheduler::do_run_one`). TSAN 빌드 활성화 후 false positive를 개별 확인하여 정밀 suppression으로 교체. **[FSD 분석 2026-03-22]** TSAN 빌드 실행 후 실제 false positive 리포트 분석이 선행 필수. 자동화 불가.

### #140. TcpAcceptor bind_address 설정 가능화
- **등급**: MAJOR
- **스코프**: CORE
- **타입**: SECURITY
- **연관**: #141
- **설명**: `TcpAcceptor`가 `tcp::v4()` (0.0.0.0)에 하드코딩 바인딩. `ServerConfig`에 `bind_address` 필드가 없어 내부 서비스(auth-svc, chat-svc)가 불필요하게 외부 네트워크에 노출될 수 있음. Gateway만 외부 바인딩, 나머지는 `127.0.0.1` 기본값 권장.

### #141. TCP 동시 연결 수 제한 (max_connections)
- **등급**: MAJOR
- **스코프**: CORE
- **타입**: SECURITY
- **연관**: #140
- **설명**: `TcpAcceptor`에 최대 동시 연결 수 제한 없음. 악의적 대량 연결 시 서버 리소스(fd, 메모리) 고갈 DoS 벡터. Rate limit은 Gateway 메시지 단위만 존재. `ServerConfig`에 `max_connections` 필드 추가 + 임계치 초과 시 신규 연결 거부.

### #139. auth_logic.hpp is_account_locked 타임존 비교 부정확
- **등급**: MAJOR
- **스코프**: AUTH_SVC
- **타입**: BUG
- **연관**: #128(HISTORY)
- **설명**: `is_account_locked()`가 UTC 포맷 문자열과 PostgreSQL `timestamptz`(타임존 오프셋 포함 가능) 문자열을 사전순 비교. 오프셋(예: `+09`)이 포함되면 시간 순서와 불일치하여 계정 잠금 판정 오류 가능. 권장: SQL에서 `locked_until > NOW()` 비교로 전환, 또는 C++에서 타임존 파싱 수행.

### #137. KafkaConsumer 소멸자 경로 handler lifetime 보호
- **등급**: MAJOR
- **스코프**: SHARED
- **타입**: DESIGN_DEBT
- **연관**: #136
- **설명**: `KafkaAdapter::~KafkaAdapter()` 소멸자에서 `stop_consuming()` 후 `consumers_.clear()` 사이에 이미 큐잉된 async handler가 소멸된 KafkaConsumer에 접근 가능. `do_close()` 경로는 shutdown 시퀀스로 보호되나 소멸자 방어 경로에서는 미보호. 비정상 종료 또는 테스트 경로에서 간헐적 크래시 가능.

### #136. HiredisAsioAdapter `[this]` raw 캡처 — sentinel 패턴 전환
- **등급**: MAJOR
- **스코프**: SHARED
- **타입**: DESIGN_DEBT
- **연관**: #137
- **설명**: `handle_read()/handle_write()`의 `async_wait` 콜백이 `[this]`를 raw 캡처. `RedisConnection::disconnect()` → `redisAsyncFree()` → `on_cleanup()` → `socket_.cancel()` + `socket_.release()` → `asio_adapter_.reset()` 순서로 실행 시, 취소된 핸들러가 io_context에 post되어 소멸 후 실행될 수 있다. 현재는 `!ec` short-circuit으로 안전하지만 취약한 구조. `shared_ptr<bool>` sentinel 또는 `shared_from_this` 패턴으로 전환 권장.

### #142. CrashHandler 단위 테스트 부재
- **등급**: MAJOR
- **스코프**: CORE
- **타입**: TEST
- **연관**: #4(HISTORY)
- **설명**: `crash_handler.cpp`의 signal handler 설치/해제 및 크래시 시 로깅 동작이 미검증. 프로세스 전역 상태 변경이므로 fork/subprocess 기반 테스트 필요. Windows SEH 핸들러 테스트도 고려 대상. 구현 비용이 높으므로 IN VIEW 배치.

### #143. AdapterBase::spawn_adapter_coro() DRAINING 거부 테스트
- **등급**: MAJOR
- **스코프**: SHARED
- **타입**: TEST
- **설명**: `test_adapter_base.cpp`에서 init/drain/close 라이프사이클 상태 전이는 검증하지만, `spawn_adapter_coro()`가 DRAINING/CLOSED 상태에서 코루틴 spawn을 올바르게 거부하는지 미검증. 어댑터 코루틴 관리의 핵심 경로이며, drain 시 새 코루틴이 spawn되면 리소스 누수 발생 가능. mock adapter에서 호출 형태로 비용 낮게 구현 가능.

### #146. AuthService bcrypt 검증이 코루틴 스레드에서 동기 블로킹
- **등급**: MAJOR
- **스코프**: AUTH_SVC
- **타입**: PERF
- **설명**: `password_hasher_.verify()`가 bcrypt 해시 비교(work factor 12, ~250ms)를 코어 스레드에서 동기 실행. 해당 코어의 모든 비동기 작업이 250ms 블로킹됨. 동시 로그인 요청 폭주 시 코어 처리량 급격 저하. thread pool offload 또는 `co_spawn` 분리 필요.

### #147. Docker 서비스 이미지에 테스트용 RSA 키 번들링
- **등급**: MAJOR
- **스코프**: INFRA
- **타입**: SECURITY
- **연관**: #140
- **설명**: gateway/auth-svc Dockerfile의 runtime 스테이지에서 `COPY apex_services/tests/keys/ /app/keys/`로 E2E 테스트 전용 RSA 키를 이미지에 포함. 현재 Dockerfile이 E2E 전용이라 실질 위험 없으나, 프로덕션 Dockerfile 파생 시 테스트 키 배포 위험. 프로덕션은 Kubernetes Secret/Docker Secret 외부 마운트 필수.

### #148. 테스트 커버리지 갭 — 핵심 경로 9건
- **등급**: MAJOR
- **스코프**: CORE, SHARED
- **타입**: TEST
- **설명**: auto-review에서 식별된 미테스트 경로: ① AdapterBase init 실패 복구 ② CircuitBreaker 동시 호출 ③ CrossCoreCall 동일 코어 호출 ④ Session 동시 write ⑤ TimingWheel 용량 1 ⑥ FrameCodec UnsupportedProtocolVersion ⑦ RedisMultiplexer close 중 콜백 안전성 ⑧ ConfigTest num_cores=0 ⑨ SpscMesh 자기 코어 post. 우선순위: ①⑦(프로덕션 장애 직결) > ②③(동시성) > 나머지.

### #149. Whisper core_id=0 하드코딩 — SessionId core 인코딩
- **등급**: MINOR
- **스코프**: CHAT_SVC, GATEWAY
- **타입**: DESIGN_DEBT
- **설명**: Whisper unicast 전송 시 `core_id=0` 하드코딩. auto-review L1 수정으로 모든 코어 순회 방식으로 동작하나 O(N_cores) 비용. SessionId에 core_id를 인코딩하면 단일 코어 post로 O(1) 전달 가능.

### #65. auto-review 가이드 검증 자동화
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **연관**: #1(HISTORY), #126(HISTORY)
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제. **[FSD 설계 확정 2026-03-22]** C안 채택: #126 Go 백엔드에 리뷰 검증 기능 통합. #126 완료(2026-03-23), 착수 가능.


---

## DEFERRED

### #59. 문서 자동화 — 생성 스크립트 + pre-commit 검증 + 템플릿
- **등급**: MAJOR
- **스코프**: TOOLS, DOCS
- **타입**: INFRA
- **설명**: 에이전트가 문서 규칙을 무시하는 문제를 규칙이 아닌 코드로 강제. 5가지 자동화: ① `new-doc.sh` — category/project/version/topic 인자, `date` 기반 타임스탬프, 시스템 시간 sanity check, etc 경로 WARNING + 머지 전 보고. ② superpowers 파일 차단 — `.gitignore` + pre-commit hook 워킹 디렉토리 스캔, 커밋 실패 + 재작성 안내. ③ 빈 문서 차단 — pre-commit hook N줄 미만 reject. ④ 타임스탬프 사후 보정 — `fix-doc-timestamps.sh` git log 대조 + 신규 파일 현재 시각 비교. ⑤ 카테고리별 `.template.md`. **후순위 강등(2026-03-22)**: 현재 스크립트 후킹으로 문서 규칙 준수율 충분. #126 Go 백엔드 완료(2026-03-23). 재평가 가능.

### #50. apex_tools/scripts 폴더 신설 + 스크립트 정리
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **연관**: #126(HISTORY)
- **설명**: 독립 실행형 스크립트 3종을 `apex_tools/scripts/`로 이동. 경로 민감 스크립트는 유지. **[FSD 분석 2026-03-22]** #126 Go 백엔드 재작성 완료(2026-03-23)로 bash 스크립트 5종 삭제됨. 잔여 스크립트 유무 재평가 후 착수 여부 결정.

### #144. Session::state_ TSAN 호환성 — non-atomic cross-thread 접근
- **등급**: MINOR
- **스코프**: CORE
- **타입**: DESIGN_DEBT
- **설명**: `Session::state_`가 non-atomic `State` 타입. per-core single-threaded io_context로 보호되지만, `~Session()` 소멸자가 코어 외부 스레드에서 호출될 가능성이 있으며(session.hpp:66-67 주석) close()를 호출하므로 `state_` data race 가능. TSAN false positive 발생 시 실제 race 탐지가 어려워질 수 있음. `std::atomic<State>`로 변경 권장.

### #145. on_leave_room SISMEMBER+SREM TOCTOU 레이스
- **등급**: MINOR
- **스코프**: CHAT_SVC
- **타입**: DESIGN_DEBT
- **연관**: #105(HISTORY)
- **설명**: SISMEMBER + SREM이 별도 명령이므로 동일 유저의 동시 leave 요청에서 이론적 TOCTOU race 가능. #105에서 join_room은 Lua 스크립트로 원자화했으나 leave는 미적용. SREM이 idempotent하므로 실질적 피해 미미. 동일 패턴으로 Lua 스크립트화하면 해결.

### #36. Acceptor core 0 부하 불균형
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: 단일 acceptor core 0 집중. per-core acceptor 검토. **[FSD 분석 2026-03-22]** per-core acceptor 설계 + 벤치마크 비교 필요. 자동화 불가.

### #40. NUMA 바인딩 + Core Affinity
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: v1.0+ 멀티 소켓 배포 시 재평가. **[FSD 분석 2026-03-22]** v1.0+ 전제 조건 미충족. 트리거 대기.

### #41. mmap 직접 사용 (malloc 대체)
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: v0.6 RSS 모니터링 도입 시 재평가. **[FSD 분석 2026-03-22]** v0.6 RSS 모니터링 전제 조건 미충족. 트리거 대기.

### #42. Hugepage (대형 페이지)
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: 부하 테스트에서 TLB miss 병목 확인 후. **[FSD 분석 2026-03-22]** TLB miss 병목 확인 전제 조건 미충족. 트리거 대기.

### #43. L1 로컬 캐시
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: v1.0+ 이후. **[FSD 분석 2026-03-22]** v1.0+ 전제 조건 미충족. 트리거 대기.

### #44. 코루틴 프레임 풀 할당 (ADR-21)
- **등급**: MINOR
- **스코프**: CORE
- **타입**: PERF
- **설명**: 벤치마크에서 병목 확인 시 도입. **[FSD 분석 2026-03-22]** 벤치마크 병목 확인 전제 조건 미충족. 트리거 대기.


