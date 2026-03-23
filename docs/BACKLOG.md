# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 161

---

## NOW

---

## IN VIEW

### #133. TransportContext의 ssl::context* — apex_core에 OpenSSL 직접 의존
- **등급**: MAJOR
- **스코프**: CORE, SHARED
- **타입**: DESIGN_DEBT
- **연관**: #130(이번PR에서도입)
- **설명**: `TransportContext`가 `boost::asio::ssl::context*`를 직접 보유하여 apex_core에 OpenSSL 의존이 발생. **[FSD 설계 확정 2026-03-22]** B안 채택: Virtual SocketBase wrapper. Session이 `unique_ptr<SocketBase>` 보유, TcpSocket/TlsSocket 구현체. ssl::context는 Listener<P, TlsTcpTransport>가 소유. Session/SessionManager 비템플릿 유지. 상세: `docs/apex_common/plans/20260322_162133_fsd_design_decisions_batch.md` **[FSD 분석 2026-03-23]** SocketBase virtual interface + TcpSocket/TlsSocket 구현체 + Session 소유권 변경 등 다중 파일 대규모 리팩터링. 설계는 확정되었으나 구현 범위가 FSD 자동화 한계 초과.

### #135. KafkaSecurityConfig 시크릿 처리 — sasl_password 평문 저장
- **등급**: MINOR
- **스코프**: SHARED
- **타입**: SECURITY
- **연관**: #131(이번PR에서도입)
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

### #137. KafkaConsumer 소멸자 경로 handler lifetime 보호
- **등급**: MAJOR
- **스코프**: SHARED
- **타입**: DESIGN_DEBT
- **연관**: #136
- **설명**: `KafkaAdapter::~KafkaAdapter()` 소멸자에서 `stop_consuming()` 후 `consumers_.clear()` 사이에 이미 큐잉된 async handler가 소멸된 KafkaConsumer에 접근 가능. `do_close()` 경로는 shutdown 시퀀스로 보호되나 소멸자 방어 경로에서는 미보호. 비정상 종료 또는 테스트 경로에서 간헐적 크래시 가능.

### #142. CrashHandler 단위 테스트 부재
- **등급**: MAJOR
- **스코프**: CORE
- **타입**: TEST
- **연관**: #4(HISTORY)
- **설명**: `crash_handler.cpp`의 signal handler 설치/해제 및 크래시 시 로깅 동작이 미검증. 프로세스 전역 상태 변경이므로 fork/subprocess 기반 테스트 필요. Windows SEH 핸들러 테스트도 고려 대상. 구현 비용이 높으므로 IN VIEW 배치.

### #146. apex-agent 시스템 현황 대시보드 프론트엔드
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

### #150. JWT uid 파싱 — std::stoull 예외 시 에러 메시지 부정확
- **등급**: MINOR
- **스코프**: GATEWAY, AUTH_SVC
- **타입**: BUG
- **설명**: `jwt_verifier.cpp`와 `jwt_manager.cpp`에서 `std::stoull(decoded.get_payload_claim("uid").as_string())`이 비정상 문자열에 예외를 던지면 상위 catch에서 "JWT unexpected error"로 뭉뚱그려짐. `std::from_chars`로 전환하면 예외 없이 정확한 에러("uid claim 파싱 실패")를 로깅할 수 있다.

### #151. FrameCodec vs Protocol concept 에러 타입 이원화 통일
- **등급**: MINOR
- **스코프**: CORE
- **타입**: DESIGN_DEBT
- **설명**: `FrameCodec::try_decode()`는 `expected<Frame, FrameError>`를, `Protocol` concept은 `Result<Frame>` (`expected<Frame, ErrorCode>`)을 요구. `TcpBinaryProtocol`에서 switch-case 변환이 필요하고, 새 프로토콜 구현 시 변환 누락 위험. `FrameCodec`이 직접 `ErrorCode`를 반환하면 인터페이스 통일.

### #152. ServiceBase on_start/on_stop CRTP 비대칭 미문서화
- **등급**: MINOR
- **스코프**: CORE
- **타입**: DOCS
- **설명**: `on_configure()/on_wire()/on_session_closed()`는 virtual인데 `on_start()/on_stop()`은 CRTP 디스패치. 의도적 설계(devirtualization 보장)라면 주석으로 사유를 명시해야 유지보수자 혼동 방지.

### #149. Whisper core_id=0 하드코딩 — SessionId core 인코딩
- **등급**: MINOR
- **스코프**: CHAT_SVC, GATEWAY
- **타입**: DESIGN_DEBT
- **설명**: Whisper unicast 전송 시 `core_id=0` 하드코딩. auto-review L1 수정으로 모든 코어 순회 방식으로 동작하나 O(N_cores) 비용. SessionId에 core_id를 인코딩하면 단일 코어 post로 O(1) 전달 가능. **[FSD 설계 확정 2026-03-23]** SessionId 인코딩 변경 없이 기존 MetadataPrefix의 core_id/session_id 필드 활용: ① Auth SessionStore에 core_id 함께 저장 ② Chat whisper에서 target의 session_id+core_id 모두 조회 ③ ResponseDispatcher에서 corr_id==0이면 session_id 기반 직접 전달 분기(기존 #138 설계 계승).

### #65. auto-review 가이드 검증 자동화
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **연관**: #1(HISTORY), #126(HISTORY)
- **설명**: 코어 인터페이스 변경 시 `apex_core_guide.md` 갱신 누락을 auto-review 스크립트에서 자동 탐지. CLAUDE.md 유지보수 규칙의 "머지 전 체크" 항목을 코드 레벨로 강제. **[FSD 설계 확정 2026-03-22]** C안 채택: #126 Go 백엔드에 리뷰 검증 기능 통합. #126 완료(2026-03-23), 착수 가능. **[FSD 분석 2026-03-23]** Go 백엔드 기능 추가로 C++ 백로그 번들과 스코프 상이. 별도 작업으로 착수 권장.

### #154. apex-agent git CLI 서브커맨드 그룹
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: INFRA
- **설명**: apex-agent에 `git` 서브커맨드 그룹 추가 (checkout-main, switch, rebase). validate-build hook이 git 브랜치 생성을 차단하므로, 데몬 기반 안전한 git 조작 경로 필요. 나중에 핸드오프 검증 연동(switch 시 해당 브랜치의 handoff 상태 확인) 가능.

### #155. ModuleLogger .With() 매 호출 allocation 개선
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: PERF
- **설명**: ModuleLogger.Debug/Info/Warn/Error가 매 호출마다 slog.Default().With("module", ml.name)으로 새 Logger를 생성. hot path 아니지만 sync.Once 기반 lazy init 또는 slog.LogAttrs로 allocation 절감 가능.

### #156. Queue polling exponential backoff
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: PERF
- **설명**: Queue.Acquire에서 락 획득 실패 시 500ms 고정 sleep. exponential backoff(100ms→200ms→400ms→max 2s) 또는 context.WithTimeout 전체 대기 시간 제한 추가.

### #157. git 명령어 에러 핸들링 — rebase abort 에러 무시
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: EnforceRebase에서 rebase 실패 시 --abort 호출의 에러를 무시(//nolint:errcheck). abort 자체가 실패하면(dirty working tree 등) 반쪽짜리 rebase 상태에 빠질 수 있음. 최소 경고 로그 기록 필요.

### #159. CLI 전체 워크플로우 workflow 패키지 이관 — HTTP 대시보드 기틀
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: INFRA
- **연관**: #146, #154
- **설명**: 현재 start/merge/drop만 workflow 패키지로 추출됨. 나머지 CLI 기능(design, plan, backlog CRUD, queue, cleanup, ack 등)도 전부 workflow 기반 파이프라인으로 추상화하여 HTTP 대시보드(BACKLOG-146)에서 동일 기능 수행 가능하도록 기틀 마련. IPCFunc 패턴 확장, 각 커맨드별 파이프라인 함수 작성, 단위+E2E 테스트 동반.

### #160. backlog UpdateFromImport에 title 갱신 누락
- **등급**: MAJOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: `UpdateFromImport()`가 severity, timeframe, scope, type, description, related, position은 갱신하지만 title을 갱신하지 않음. import 전에 동일 ID로 `backlog add`가 실행되면 title이 오염된 채 남음. title도 메타데이터이므로 import 시 MD 기준으로 갱신해야 한다.

### #158. Plugin 시스템 Claude Code 포맷 버전 체크 부재
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **설명**: plugin.Setup()이 ~/.claude/ 디렉토리 구조, installed_plugins.json, settings.json 포맷에 직접 의존. 포맷 변경 시 무조건 깨짐. 파일 포맷 버전 체크를 추가하여 호환성 깨질 때 명시적 에러 반환. ---


---

## DEFERRED

### #59. 문서 자동화 — 생성 스크립트 + pre-commit 검증 + 템플릿
- **등급**: MAJOR
- **스코프**: TOOLS, DOCS
- **타입**: INFRA
- **설명**: 에이전트가 문서 규칙을 무시하는 문제를 규칙이 아닌 코드로 강제. 5가지 자동화: ① `new-doc.sh` — category/project/version/topic 인자, `date` 기반 타임스탬프, 시스템 시간 sanity check, etc 경로 WARNING + 머지 전 보고. ② superpowers 파일 차단 — `.gitignore` + pre-commit hook 워킹 디렉토리 스캔, 커밋 실패 + 재작성 안내. ③ 빈 문서 차단 — pre-commit hook N줄 미만 reject. ④ 타임스탬프 사후 보정 — `fix-doc-timestamps.sh` git log 대조 + 신규 파일 현재 시각 비교. ⑤ 카테고리별 `.template.md`. **후순위 강등(2026-03-22)**: 현재 스크립트 후킹으로 문서 규칙 준수율 충분. #126 Go 백엔드 완료(2026-03-23). 재평가 가능. **[FSD 분석 2026-03-23]** 5가지 자동화 시스템(스크립트 + pre-commit hook + 템플릿) 대규모 인프라 구축. 자동화 불가.

### #50. apex_tools/scripts 폴더 신설 + 스크립트 정리
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: INFRA
- **연관**: #126
- **설명**: 독립 실행형 스크립트 3종을 `apex_tools/scripts/`로 이동. 경로 민감 스크립트는 유지. **[FSD 분석 2026-03-22]** #126 Go 백엔드 재작성 완료(2026-03-23)로 bash 스크립트 5종 삭제됨. 잔여 스크립트 유무 재평가 후 착수 여부 결정.

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

### #153. apex-agent 백로그 동기화 — 단순 BACKLOG.md 수정 시 SQLite 미갱신
- **등급**: MINOR
- **스코프**: TOOLS
- **타입**: BUG
- **설명**: `docs/BACKLOG.md`를 직접 수정(항목 추가/삭제/편집)해도 apex-agent Go 백엔드의 SQLite DB에 반영되지 않음. 다음 에이전트가 `apex-agent backlog import`/`export`를 실행해야 동기화됨. 단순 BACKLOG.md 수정 시에도 정식 import/export 워크플로우가 자동 트리거되어야 함. pre-commit hook 또는 파일 감시 기반 자동 동기화 검토 필요.


