# apex_core 프레임워크 설계 결정사항

브레인스토밍 완료 (2026-03-06). 5차 재검토 통과.

---

## 확정된 결정

### 모듈 정체성
- **서버 프레임워크** (라이브러리 X)
- ServiceBase<Derived> CRTP 패턴으로 서비스 정의
- 프레임워크가 실행 흐름 소유, 서비스는 핸들러만 등록
- 인간 가독성 + AI 에이전트 유지보수 용이한 구조 지향

### C++ 표준
- **C++23** (co_await 코루틴, concepts 활용)

### 성능 철학
- 런타임 비용을 최대한 컴파일 타임으로 이동
- CRTP 정적 다형성 (핫패스에서 vtable 회피, 콜드패스인 서비스 생명주기 관리는 virtual 허용)
- 캐시 친화적, SoA 허용
- 하드웨어 친화적 설계 전반 적용
- **shared-nothing 범위**: 비즈니스 로직 코어에 적용. 외부 라이브러리 내부 스레드(librdkafka, spdlog, prometheus-cpp)는 예외
- **핫패스 malloc 제거**: 프레임워크 핫패스에서는 슬랩 풀 할당만 사용 (외부 라이브러리 내부 할당은 통제 불가)

### 이벤트 루프
- **io_context-per-core (shared-nothing)**
- 각 코어가 자기만의 이벤트 루프 + 커넥션 + 메모리 풀 소유
- SO_REUSEPORT로 커널이 코어별 클라이언트 분배
- 코어 간 통신: **코어당 MPSC 수신 큐 1개** (N코어 = N큐, 관리 단순)
  - 브로드캐스트 (채팅 등): 대상 코어들의 MPSC 큐에 각각 enqueue
  - Kafka 응답 라우팅: 목적지 세션이 있는 코어의 MPSC 큐로 전달
  - 관리 명령 (shutdown, 로그 레벨 변경): 전체 코어 MPSC 큐에 브로드캐스트
  - **요청-응답 패턴**: MPSC 위에 요청 ID + 응답 콜백 래퍼 (cross_core_call → awaitable<T>)

### 비동기 I/O 통합 (모든 I/O를 Asio 이벤트 루프 위에)
- TCP/WebSocket: Boost.Asio 네이티브
- Kafka: librdkafka fd를 Asio에 등록 (io_event_enable)
- Redis: redis-plus-plus async (Asio 백엔드)
- PostgreSQL: libpq fd를 Asio에 등록 (PQsocket)
- 프레임워크는 어댑터를 제공하되 통신 경로를 강제하지 않음

### Kafka와 shared-nothing 원칙
- Kafka는 shared-nothing의 **현실적 예외**
- **KafkaProducer**: 전역 공유 인스턴스 1개 (librdkafka 내부가 이미 스레드세이프, 브로커별 커넥션 자동 관리)
  - 각 코어 → SPSC 큐 → 공유 Producer로 전달
  - 싱글톤 패턴 아닌, 프레임워크 초기화 시 생성 후 참조 전달 (테스트 용이성)
- **KafkaConsumer**: 파티션:코어 매핑으로 자연스럽게 분리
  - Kafka consumer group이 파티션을 코어에 자동 분배
- DB와 달리 Proxy/커넥션 풀 불필요 (Kafka 커넥션은 영속 TCP, 브로커 수에 비례하는 고정 수량)

### DBMS
- **PostgreSQL** (MySQL에서 변경)
- 이유: 고가용성 아키텍처 + 업계 트렌드 + MVCC 우수
- DB 커넥션 폭증 문제: **DB Proxy로 해결** (PgBouncer/Odyssey/PgCat 중 나중에 선택)
- libpq → Asio 어댑터 직접 구현 (포트폴리오 가치)

### 직렬화
- **FlatBuffers** (Protobuf에서 변경)
- 이유: zero-copy 읽기, 역직렬화 비용 제로, 게임업계 검증
- gRPC 제거로 Protobuf 강제 사유 소멸
- 크로스 플랫폼 호환 (little-endian + vtable 오프셋)
- Serializer 추상 레이어 없음 (YAGNI)

### 와이어 프로토콜
- **TcpBinaryProtocol** + **WebSocketProtocol** 두 가지 제공
- CRTP ProtocolBase<Derived>로 추상화, vtable 없음
- 유저가 서비스 정의 시 프로토콜 선택
- 프레이밍: 고정 헤더 (ver uint8 + msg_id uint16 + body_size uint32 + flags uint16 + reserved uint8) + FlatBuffers 페이로드
- **프로토콜 버전 관리**: 헤더의 ver 필드로 클라이언트 버전 식별
  - FlatBuffers 필드 추가는 하위 호환 (새 필드는 기본값)
  - 비호환 변경 시 ver 올리고 Gateway가 구버전 클라이언트에 업데이트 응답

### 메시지 디스패치
- **route<T> 기반 하이브리드 디스패치**
  - 등록: `route<LoginRequest>(MsgId::Login, &MyService::on_login)`
  - 내부: std::array<Handler, 65536> 인덱스 직접 접근 O(1)
  - 핸들러 시그니처에서 FlatBuffers 타입 강제 (타입 안전)
  - msg_id와 FlatBuffers 타입 불일치 시 컴파일 에러

### 에러 핸들링
- **하이브리드 전략**
  - 핫패스 (메시지 수신/파싱/디스패치): std::expected<T, Error> (zero-cost happy path)
  - 콜드패스 (초기화, 설정 오류): 예외 throw
  - 복구 불가능 (메모리 고갈): 예외 전파 (`std::bad_alloc` throw → 호출자 처리 위임)
- **에러 전파**: 기본 자동 + 오버라이드 가능
  - 핸들러에서 `co_return apex::error(ErrorCode::X)` → 프레임워크가 자동 ErrorResponse 전송
  - 커스텀 필요 시 직접 `session.send()` 후 `co_return apex::ok()`
  - 핸들러가 에러를 리턴했는데 응답을 안 보냈으면 자동 ErrorResponse 생성

### 코루틴 + 세션 수명 안전
- **세션을 shared_ptr로 관리**, 코루틴이 shared_ptr을 캡처하여 수명 보장
- co_await 중 클라이언트 연결 끊김 → 세션 객체는 코루틴 종료까지 유지
- 이미 끊긴 세션에 send 시 graceful하게 무시 (크래시 방지)
- 프레임워크가 강제 — 서비스 개발자가 수명 관리를 신경 쓸 필요 없음

### 코루틴 프레임 할당 최적화
- C++20 코루틴은 프레임마다 **힙 할당 발생** (수백 바이트~수 KB)
- **고성능 범용 allocator(mimalloc/jemalloc) + HALO 컴파일러 최적화** 활용
- 벤치마크에서 병목 확인 시 커스텀 코루틴 타입 도입 검토
- 업계 주요 프레임워크(Seastar, folly, Boost.Asio, cppcoro) 조사 결과, promise_type 풀 오버로드는 미검증 접근

### Zero-copy 범위 명시
- FlatBuffers 페이로드 읽기: **zero-copy** (링 버퍼에서 직접 포인터 접근)
- 링 버퍼 wrap-around 시: 메시지가 경계에 걸리면 **예외적으로 연속 버퍼에 copy**
- 대부분(99%+) 메시지는 연속 영역에 위치하므로 실질적 zero-copy

### 백프레셔
- MPSC 큐에 **max_capacity 제한**
- enqueue 실패 시 `std::expected<void, QueueError>` 반환
- 큐 80% 도달 시 Gateway에 슬로우다운 시그널 → 클라이언트에 429 응답
- Kafka 토픽은 디스크 기반이라 자연스러운 버퍼 역할

### 하드웨어 친화적 컴포넌트
- MPSC 락프리 큐 (캐시 라인 패딩, acquire/release, 용량 제한)
- 슬랩 메모리 풀 (코어별 독립, 타입별, 할당 O(1))
  - 확장 포인트: 고수위/저수위 정책으로 유휴 슬랩 OS 반환 (1차 YAGNI, 벤치마크 후 필요 시 추가)
- 링 버퍼 (수신 버퍼, memmove 제거)
- FlatBuffers → 이미 zero-copy (별도 Arena 불필요)
- 배치 I/O (writev)
- alignas(64) 멀티스레드 공유 구조에 적용 (MPSC 큐 등 코어 간 공유 데이터의 false sharing 제거, per-core 전용 컴포넌트는 대상 아님)

### I/O 백엔드 전략
- **기본: epoll** (안정적, 컨테이너 호환, 네트워크 I/O에서 충분한 성능)
- **선택: io_uring** (CMake 옵션 APEX_USE_IO_URING, 기본 OFF)
  - 네트워크 I/O에서는 epoll 대비 10~20% 개선에 그침
  - 파일 I/O (로그 등)에서는 진짜 비동기로 이점 큼
  - Docker 기본 seccomp 프로파일이 io_uring 시스콜 차단 → 커스텀 seccomp 필요
  - 활성화 요건: Linux 5.11+, liburing, 커스텀 seccomp 프로파일
  - BOOST_ASIO_HAS_IO_URING은 컴파일 타임 스위치 (런타임 선택 불가)

### 플랫폼 분기
- **SO_REUSEPORT**: Linux 전용 → Windows fallback (단일 acceptor + round-robin)
- **시그널 (SIGTERM, SIGHUP)**: Linux 네이티브 → Windows는 등가 메커니즘으로 대체
- 개발은 Windows, 성능 테스트/배포는 Linux Docker

### 빌드 & 인프라
- CMake + Ninja (크로스 플랫폼 고속 빌드)
- vcpkg (의존성 관리, vcpkg.json 선언형)
- Docker 컨테이너화 (빌드 환경 표준화, 멀티스테이지)
- CMakePresets.json으로 원커맨드 빌드 (`cmake --preset default`)

### 형상 관리
- **GitHub Flow** (main + feature 브랜치, PR 기반 머지)
- **Semantic Versioning** 태깅 (v0.1.0, v0.2.0, ... v1.0.0)
- PR 단위로 작업 기록 → 포트폴리오 가치 + 나중에 협업 확장 가능
- GitHub Actions CI: PR마다 자동 빌드/테스트

### 로깅/모니터링
- **spdlog** + 커스텀 KafkaSink (중앙 집중 로그 파이프라인)
- 구조화 JSON 포맷
- Sink 구성: ConsoleSink(개발), FileSink(로컬 백업), KafkaSink(프로덕션)
- Prometheus 메트릭 노출 인터페이스 (카운터/게이지)
- 로그 전용 Kafka 토픽 분리 (비즈니스 메시지와 대역폭 경쟁 방지)
- 참고: spdlog 비동기 모드의 내부 스레드는 로그 flush 전용이므로 shared-nothing 원칙과 충돌하지 않음

### 분산 추적 (Distributed Tracing)
- **trace_id 기반 요청 추적** (OpenTelemetry 경량 자체 구현)
  - Gateway에서 trace_id 생성 (요청 진입점)
  - 세션 컨텍스트에 자동 주입, Kafka 메시지 헤더에 전파
  - 로그 출력 시 trace_id 자동 첨부 (서비스 개발자가 의식할 필요 없음)
  - Kibana에서 trace_id 검색 → 전체 서비스의 관련 로그가 시간순으로 조회
- 나중에 OpenTelemetry 풀스택으로 확장 가능한 구조

### 설정 관리
- **TOML** (toml++, header-only, C++17)
- 런타임/인프라/운영 설정 분리
- K8s ConfigMap에 텍스트 파일 그대로 마운트
- 런타임 설정 리로드: 전체 hot-reload는 YAGNI, **로그 레벨만 SIGHUP으로 변경 가능** (spdlog set_level() 활용)

### 테스트
- **Google Test** (단위/통합) + **Google Benchmark** (벤치마크)
- 3계층: tests/unit, tests/integration, tests/bench
- **TSAN (Thread Sanitizer)** 필수 — MPSC 큐 등 락프리 자료구조 동시성 검증
- **ASAN (Address Sanitizer)** 필수 — 코루틴 수명, 메모리 풀 정합성 검증

### 디렉토리 구조
- **모노레포** (apex-pipeline/)
- 네임스페이스: `apex` (apex::core, apex::gateway, apex::auth 등)
- core/가 apex_core 프레임워크
- examples/에 프레임워크 사용 예제 포함
- 서비스들은 CMake find_package(ApexCore)로 의존

### Graceful Shutdown
- SIGTERM → acceptor 중지 → 코어별 세션 close → 세션 drain 폴링(active_sessions==0 대기) → CoreEngine stop → drain_remaining → join → 서비스 정지 → 종료
- drain 타임아웃: 설정 가능, 기본값 25초 (K8s 30초 대비 5초 여유) (Phase 5에서 구현)

### 세션 관리
- **코어 로컬 해시맵 + Redis 백업** (세션 상태 저장)
- **타이머 휠 + 양방향 하트비트** (타임아웃 관리)
  - 코어별 타이머 휠 1개, O(1) 타임아웃
  - 클라이언트가 주기적 전송 + 서버도 무응답 시 ping
  - WebSocket: 프로토콜 내장 ping/pong 활용
  - TCP: 프레임워크 레벨 heartbeat 메시지 타입

### 보안
- **Gateway TLS 종단** (내부 통신은 평문)
- **JWT + 로컬 블룸필터 + Redis 블랙리스트** (인증)
  - JWT 서명 검증 (로컬) → 블룸필터 체크 (로컬) → 필요 시만 Redis 조회
  - Auth 서비스가 로그아웃 시 Redis Pub/Sub로 블룸필터 갱신 브로드캐스트
  - 블룸필터 갱신 타이밍 갭 (수 ms): JWT 모델의 본질적 한계이므로 허용
  - 보안 민감 작업 (결제 등): 메시지 타입별 강제 Redis 검증 플래그로 대응

### 개발 편의
- **docker-compose 프로파일**: 기본(Kafka,Redis,PG — 프로파일 없이 항상 실행) / observability(+Prometheus,Grafana) / full(향후)
- **서비스 스캐폴딩**: tools/new-service.sh로 보일러플레이트 자동 생성
- **외부 의존성**: Boost, FlatBuffers, librdkafka, redis-plus-plus, libpq, spdlog, prometheus-cpp, toml++, jwt-cpp, GTest, GBenchmark (전부 vcpkg) — v0.2.4 현재 boost-asio, boost-beast, flatbuffers, gtest만 사용 중, 나머지는 해당 Phase에서 추가 (Kafka/spdlog → Phase 6, Redis/libpq → Phase 7, jwt-cpp/prometheus-cpp → Phase 8~9)

---

## 구현 로드맵 (에이전트 팀 병렬 기반)

작업 원칙:
- 1 세션 = 파일 1~3개 + 해당 테스트 + 테스트 통과 확인
- 병렬 가능 조건: 파일 비겹침, 인터페이스 사전 정의, 의존성 없음
- 각 세션 종료 시 docs/apex_common/progress/ 에 체크포인트 문서 작성
- 패턴: 병렬 구현 → 단일 통합 → 태그

### Phase 1: 프로젝트 셋업 (단일 세션)
- CMake 구조, vcpkg.json, 디렉토리 스캐폴딩
- 모든 컴포넌트의 **헤더 인터페이스 사전 정의** (병렬 작업의 계약)

### Phase 2: 기반 컴포넌트 (에이전트 팀 4병렬)
- Agent A: MPSC 락프리 큐 + GTest
- Agent B: 슬랩 메모리 풀 + GTest
- Agent C: 링 버퍼 + GTest
- Agent D: 타이머 휠 + GTest

### Phase 3: 코어 프레임워크 (에이전트 팀 3병렬)
- Agent A: io_context-per-core 엔진 + 테스트
- Agent B: ServiceBase<T> CRTP + route<T> 디스패치 + 테스트
- Agent C: 와이어 프로토콜 (TCP/WebSocket) + 테스트

### Phase 3.5: 통합 (단일 세션)
- Phase 2 + 3 컴포넌트 통합
- 에코 서버 예제 + 통합 테스트 → **v0.1.0**

### Phase 4: 프로토콜 + 세션 (에이전트 팀 3병렬)
- Agent A: FlatBuffers 스키마 + 코드 생성 파이프라인
- Agent B: 세션 관리 (코어 로컬 + 타이머 휠 연동)
- Agent C: 에러 전파 (자동 ErrorResponse + apex::error)

### Phase 4.5: 통합 (단일 세션)
- 채팅 예제 + 통합 테스트 → **v0.2.0**

### Phase 5: 기반 정비 (CI/CD + 설정 + Graceful Shutdown)
- CI/CD: GitHub Actions (빌드+테스트 자동화, docker-compose 통합 테스트)
- TOML 설정: toml++ 통합, ServerConfig TOML 로딩, 설정 파일 구조
- Graceful Shutdown: TOML에서 drain_timeout 로딩, Server::stop() 적용, SIGHUP 로그 레벨

### Phase 6: Kafka 체인 (Phase 7과 병렬 가능)
- Kafka 어댑터: KafkaProducer 래퍼 (전역 공유, ADR-08), KafkaConsumer (파티션:코어 매핑), librdkafka fd → Asio
- 로깅: spdlog 통합 (Console+File+KafkaSink), 구조화 JSON, trace_id 자동 주입
- 내부 의존: Kafka 어댑터 → 로깅

### Phase 7: 데이터 체인 (Phase 6과 병렬 가능)
- Redis 어댑터: redis-plus-plus async (Asio 백엔드)
- PG 어댑터: libpq fd → Asio 등록, 비동기 쿼리 래퍼
- Connection Pool: 공통 풀 추상화, 코어별 인스턴스 (shared-nothing), health check
- 내부 의존: Redis ∥ PG → Connection Pool

### Phase 7.5: 어댑터 통합 (단일 세션)
- Phase 6 + 7 어댑터 통합 테스트 → **v0.3.0**

### Phase 8: Gateway 체인
- WebSocket 프로토콜: WebSocketProtocol (ProtocolBase CRTP), Beast 통합, ping/pong (ADR-06)
- Gateway: TLS 종단, JWT 검증 (ADR-07), 블룸필터, Kafka 라우팅, Rate Limiting
- Auth 서비스: JWT 발급/갱신, Redis 블랙리스트, 블룸필터 Pub/Sub, PG 스키마
- 내부 의존: WebSocket → Gateway ∥ Auth

### Phase 8.5: 파이프라인 통합 (단일 세션)
- E2E 통합 테스트 → **v0.4.0**

### Phase 9: 운영 인프라 (4작업 완전 병렬)
- 메트릭: prometheus-cpp, Grafana 대시보드
- Docker: 서비스별 Dockerfile (멀티스테이지)
- K8s: Helm Chart, HPA, ConfigMap, Health Check
- CI/CD 고도화: Docker 빌드 + 배포, 스캐폴딩 스크립트

### Phase 10: 최종 통합 (단일 세션)
- K8s E2E, 부하 테스트, 문서 정리 → **v1.0.0**

---

## 설계 근거
- 각 결정의 대안 분석과 선택 근거는 `design-rationale.md` (ADR) 참조
