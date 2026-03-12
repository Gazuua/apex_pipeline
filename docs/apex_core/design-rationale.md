# apex_core 설계 근거 (Architecture Decision Records)

브레인스토밍 과정에서의 대안 분석과 최종 선택 근거를 기록한다.
기존 포트폴리오 Why Document의 연장선이며, 프레임워크 세부 설계에 초점을 맞춘다.

---

## ADR-01: 로깅 전략

### 맥락
- io_context-per-core (shared-nothing) 아키텍처에서 로깅 필요
- Apex Pipeline은 중앙 Kafka 클러스터 기반 MSA 구조
- 팀 경험: JSON 로그 + Kibana 대시보드 운영 경험 있음

### 대안 분석

**A. 운영 디버깅 중심 (텍스트 로그)**
- spdlog 등으로 파일/stdout 출력
- 장애 시 grep으로 원인 추적
- 단순하고 익숙하지만, 실시간 감시 불가

**B. 메트릭/관측성 중심 (Prometheus + 트레이싱)**
- 로그 최소화, 숫자 시계열 데이터에 집중
- 실시간 대시보드 강력하지만 "무슨 일이 일어났는가"에 약함

**C. 하이브리드 (구조화 로깅 + 메트릭)**
- JSON 구조화 로그 + Prometheus 메트릭 노출
- 두 관점 모두 커버, 복잡도 약간 증가

### 결정: C (하이브리드) + Kafka 로그 파이프라인

### 근거
1. 중앙 Kafka가 이미 있으므로 로그도 Kafka 토픽으로 흘리면 추가 인프라 최소화
2. 라인플러스 사내 인프라가 동일한 패턴 (Kafka 중앙 버스 + 로그 파이프라인)
3. spdlog의 커스텀 Sink 인터페이스로 KafkaSink 구현이 자연스러움
4. Prometheus 메트릭은 프레임워크 침투량이 적음 (counter.Increment() 수준)

### 세부 결정
- **라이브러리**: spdlog (자체 Logger 구현 대신)
  - 근거: 락프리 큐/메모리 풀 등 핵심 컴포넌트는 직접 구현하되, 로깅처럼 잘 검증된 영역은 라이브러리 활용하여 관리 포인트 절감
- **Sink 구성**: ConsoleSink(개발), FileSink(로컬 백업), KafkaSink(프로덕션)
- **포맷**: 구조화 JSON
- **주의사항**: 로그 폭풍 시 비즈니스 메시지와 대역폭 경쟁 방지를 위해 로그 전용 토픽을 별도 파티션 수/retention으로 분리

---

## ADR-02: 설정 관리 포맷

### 맥락
- 서버 포트, 코어 수, 인프라 주소, 로그 레벨 등 다양한 런타임 설정 필요
- K8s ConfigMap과의 호환성 고려
- C++20 프로젝트

### 대안 분석

**A. YAML**
- K8s ConfigMap과 자연스럽게 호환
- 계층 구조 표현 우수
- 단점: 암묵적 타입 변환 함정 (`no` → `false`, `1.0` → 문자열 가능성), 들여쓰기 민감

**B. TOML**
- 타입이 명확 (숫자는 숫자, 문자열은 문자열)
- 들여쓰기 무관, 주석 가능, 섹션 구분 깔끔
- C++ 파서 toml++이 header-only, C++17 (C++20 호환)

**C. JSON**
- 로깅에서 JSON을 쓰니 포맷 통일 가능
- 단점: 주석 불가 (설정 파일에 치명적)

### 결정: B (TOML)

### 근거
1. 서버 설정에서 타입 안전성이 중요 — YAML의 암묵적 변환은 런타임 버그 원인이 될 수 있음
2. toml++이 header-only로 의존성 부담 없음
3. K8s ConfigMap에 텍스트 파일 그대로 마운트 가능하므로 호환성 문제 없음
4. 주석 지원으로 설정 파일 자체가 문서 역할

---

## ADR-03: 테스트 프레임워크

### 맥락
- 단위 테스트, 통합 테스트, 벤치마크 3계층 필요
- 팀 경험: Google Test 실무 사용 중

### 대안 분석

**A. Google Test + Google Benchmark**
- C++ 테스트의 사실상 표준
- 벤치마크도 같은 Google 생태계
- 실무 경험 있음

**B. Catch2**
- Header-only, 문법이 깔끔 (REQUIRE, SECTION)
- 벤치마크는 별도 라이브러리 필요

**C. doctest**
- Catch2보다 컴파일 빠름, 경량
- 생태계가 상대적으로 작음

### 결정: A (Google Test + Google Benchmark)

### 근거
1. 실무 경험이 있어 생산성 최고
2. 테스트와 벤치마크가 같은 생태계로 일관성 확보
3. CI/CD 연동, IDE 지원 등 인프라 성숙도 최고

---

## ADR-04: 디렉토리 구조

> **※ 현행 주석**: 아래는 초기 브레인스토밍 당시 결정 기록이다. 현재 구조는 `apex_` prefix 기반으로 재편되었으며, 최신 구조는 `docs/Apex_Pipeline.md` §7 프로젝트 구조를 참조할 것.

### 맥락
- Apex Pipeline 전체는 모노레포
- apex_core는 `core/`로 위치, 서비스들이 CMake 의존성으로 사용
- 네임스페이스 통일 필요

### 결정 (초기안)

```
apex-pipeline/
├── core/                         ← apex_core (프레임워크)
│   ├── include/apex/core/        ← 공개 헤더
│   ├── src/                      ← 구현
│   ├── tests/                    ← unit/integration/bench
│   ├── examples/                 ← 프레임워크 사용 예제 (에코 서버, 채팅 등)
│   └── CMakeLists.txt
├── gateway/                      ← Gateway 서비스
├── services/{auth,chat,log}-svc/ ← 비즈니스 서비스들
├── shared/schemas/               ← FlatBuffers 스키마
├── infra/                        ← Docker, K8s
└── docs/
```

### 근거
1. `apex` 네임스페이스로 통일하면 `apex::core`, `apex::gateway` 등 자연스러운 확장
2. 모노레포로 공유 스키마/빌드 설정 관리 용이
3. 각 서비스가 `find_package(ApexCore)` + `target_link_libraries`로 깔끔하게 의존

---

## ADR-05: Graceful Shutdown 상세

### 맥락
- K8s 롤링 업데이트 시 진행 중 요청 보호 필요
- io_context-per-core 구조에서 코어별 독립 종료 필요
- K8s terminationGracePeriodSeconds 기본값 30초

### 대안 분석 (drain 타임아웃)

**A. 10초** — 빠른 종료 우선, 미처리 건은 재연결 후 재시도
**B. 25초** — K8s 30초 내에서 여유 5초 두고 최대한 drain
**C. 설정 가능, 기본값 25초** — TOML에서 오버라이드 가능 (v0.2.0.0에서 구현)

### 결정: C (설정 가능, 기본값 25초)

### 근거
1. K8s 30초 기본값 대비 5초 여유로 안전 마진 확보
2. 환경별로 다를 수 있으므로 (개발환경은 5초로 빠르게) 설정 가능하게
3. 종료 순서: SIGTERM → acceptor 중지 → 코어별 drain → Kafka offset 커밋 → 리소스 정리 → 종료

---

## ADR-06: 세션 관리

### 맥락
- io_context-per-core + SO_REUSEPORT로 클라이언트가 특정 코어에 바인딩
- 250만 동접 경험에서 로컬 + 모니터 서버 패턴 사용
- 하트비트 기반 타임아웃 경험 있음

### 대안 분석 (상태 저장)

**A. 코어 로컬만**
- 빠르지만 해당 코어 장애 시 세션 유실
- 재접속 시 다른 코어로 가면 세션 없음

**B. 코어 로컬 + Redis 백업**
- 로컬에서 빠르게 조회, Redis로 복구 가능
- 기존 경험 (로컬 + 모니터 서버)과 동일한 패턴

### 대안 분석 (타임아웃)

**A. per-session deadline timer**
- Asio steady_timer를 세션마다 생성
- 구현 간단, 수만 커넥션까지 적합

**B. 타이머 휠 (Timing Wheel)**
- 코어별 타이머 휠 1개, O(1) 타임아웃 관리
- 대량 커넥션에 효율적
- shared-nothing과 완벽 호환 (코어별 독립, 락 불필요)

**C. 주기적 스캔**
- 일정 간격으로 전체 세션 순회
- 단순하지만 O(N)

### 결정: B + B (코어 로컬 + Redis 백업, 타이머 휠 + 양방향 하트비트)

### 근거
1. 코어 로컬 + Redis 백업은 기존 실무 패턴(로컬 + 모니터 서버)의 자연스러운 진화
2. 타이머 휠은 io_context-per-core와 찰떡 — 각 코어가 자기만의 휠을 소유하면 락이 전혀 불필요
3. Asio steady_timer 하나로 틱만 돌리면 되므로 타이머 객체는 코어당 1개
4. 하트비트 수신 시 해당 세션을 현재 틱 + timeout 위치로 이동 (O(1))

### 재검토 보완: 하트비트 방향
- **양방향**: 클라이언트가 주기적으로 전송하되, 서버도 일정 시간 무응답 시 ping 전송
- WebSocket은 프로토콜 자체에 ping/pong이 내장되어 자연스럽게 통합
- TCP 바이너리 프로토콜은 프레임워크 레벨 heartbeat 메시지 타입으로 처리

---

## ADR-07: 보안 (TLS + 인증)

### 맥락
- Gateway 패턴: 외부 TLS 종단, 내부 평문 (포트폴리오 문서에서 확정)
- 인증 토큰 검증 방식 미결정
- SSO 외부 모듈 사용 경험 있으나 인증 내부 구현 경험 없음

### 대안 분석 (토큰 검증)

**A. JWT를 Gateway에서 로컬 검증**
- 공개키로 서명 검증, Auth 서비스 호출 불필요
- 빠르지만 토큰 강제 무효화 (로그아웃) 즉시 불가

**B. 매 요청마다 Auth 서비스 조회**
- Redis에서 세션/토큰 확인
- 즉시 무효화 가능, 매번 네트워크 홉 발생

**C. JWT + Redis 블랙리스트**
- 기본은 JWT 로컬 검증 (99% 케이스, 네트워크 홉 제로)
- 로그아웃된 토큰만 Redis 블랙리스트 체크
- TTL을 토큰 만료시간과 동일하게 설정하면 자동 정리

### 결정: C (JWT + 로컬 블룸필터 + Redis 블랙리스트)

### 근거
1. 핫패스(99%)가 로컬 연산만으로 완결 — 성능 철학에 부합
2. 즉시 무효화도 가능 — Auth 서비스가 Redis에 토큰 ID 등록
3. Redis를 이미 세션 관리에 쓰므로 추가 인프라 없음
4. SSO 연동은 Auth 서비스 위에 나중에 얹는 것으로 범위 분리

### 재검토 보완: 블룸필터로 진짜 로컬 완결
- 단순 Redis 블랙리스트만으로는 매 요청마다 Redis 조회가 발생
- **로컬 블룸필터 캐시**를 두면 핫패스가 진짜로 로컬 연산만으로 완결:
  - JWT 서명 검증 (로컬) → 블룸필터 체크 (로컬, O(1))
  - "확실히 없음" (99%) → 통과, Redis 안 감
  - "있을 수도" (1%) → Redis 블랙리스트 확인
- Auth 서비스가 로그아웃 처리 시 Redis Pub/Sub로 블룸필터 갱신 브로드캐스트
- 블룸필터 갱신 타이밍 갭 (수 ms)은 JWT 모델의 본질적 한계이므로 허용
- 보안 민감 작업 (결제 등)은 메시지 타입별 강제 Redis 검증 플래그로 대응

---

## ADR-08: Kafka와 shared-nothing 원칙

### 맥락
- io_context-per-core (shared-nothing) 아키텍처에서 Kafka 어댑터를 어떻게 배치할 것인가
- librdkafka는 내부적으로 자체 스레드를 생성하며 브로커별 커넥션을 관리

### 대안 분석

**A. 코어별 독립 인스턴스**
- Producer/Consumer 모두 코어마다 생성
- shared-nothing 원칙 완벽 준수
- 단점: Kafka 브로커 커넥션이 코어 수만큼 배로 증가

**B. 전역 공유 인스턴스**
- Producer/Consumer 모두 하나를 공유
- 커넥션 절약, 하지만 shared-nothing에 예외 발생

**C. 하이브리드 — Producer 공유, Consumer 분리**
- Producer: 전역 공유 1개 (librdkafka 내부가 이미 스레드세이프)
- Consumer: 파티션:코어 매핑으로 자연스럽게 분리

### 결정: C (하이브리드)

### 근거
1. Kafka 커넥션은 DB와 근본적으로 다름
   - DB: 요청마다 커넥션 점유 → 동시 쿼리 수에 비례 → 폭증 가능 → Proxy 필요
   - Kafka: 영속 TCP 커넥션, 브로커 수에 비례하는 고정 수량 → Proxy/풀 불필요
2. Producer는 하나의 인스턴스가 배치 produce + 브로커 멀티플렉싱을 내부 처리
3. Consumer는 Kafka consumer group이 파티션을 코어에 자동 분배 → 자연스럽게 분리
4. "원칙을 알되 현실적 예외를 인지한다"는 설계 태도가 포트폴리오에서 좋은 메시지
5. 싱글톤 패턴 대신 프레임워크 초기화 시 생성 후 참조 전달 (테스트 용이성)

### 설정 리로드 보완 (ADR-02 후속)
- 전체 설정 hot-reload는 YAGNI
- **로그 레벨만 SIGHUP으로 런타임 변경 가능** (spdlog set_level() 활용)
- 장애 시 debug 레벨로 올리고 복구 후 info로 내리는 운영 시나리오 대응

---

## ADR-09: 메시지 디스패치

### 맥락
- 프레임워크가 "서비스는 핸들러만 등록"하는 구조
- FlatBuffers 메시지 타입을 식별하고 해당 핸들러로 라우팅해야 함
- 기존 경험: ENUM_CMDPACKET_LOGIN 같은 패킷별 ENUM 기반 디스패치

### 대안 분석

**A. 런타임 해시맵 (배열 인덱스)**
- msg_id(uint16_t) → std::array<Handler, 65536> 직접 접근 O(1)
- 단순하고 빠르지만, msg_id와 FlatBuffers 타입 불일치를 컴파일 타임에 못 잡음

**B. 컴파일 타임 디스패치 (템플릿 MessageMap)**
- 타입 레벨에서 매핑, switch-case를 점프 테이블로 최적화
- 타입 안전하지만 템플릿 문법이 복잡해서 사용자 경험 저하

**C. 하이브리드 (route<T> API)**
- 등록: `route<LoginRequest>(MsgId::Login, &MyService::on_login)`
- 핸들러 시그니처에서 FlatBuffers 타입 강제 (타입 안전)
- 내부는 방식 A의 배열 디스패치 (성능)
- 사용자 입장에서 가장 읽기 쉬움

### 결정: C (하이브리드 route<T>)

### 근거
1. 성능은 A와 동일 (내부가 배열 인덱싱)
2. 타입 안전성은 B와 동일 (route<T>에서 msg_id와 FlatBuffers 타입 매칭 검증)
3. 사용자 경험이 가장 좋음 — 한 줄로 읽히는 등록 API
4. "인간 가독성 + AI 에이전트 유지보수 용이" 원칙에 가장 부합

---

## ADR-10: 코어 간 통신 큐 구조

### 맥락
- io_context-per-core 아키텍처에서 코어 간 메시지 전달 필요
- 시나리오: 브로드캐스트(채팅), Kafka 응답 라우팅, 관리 명령(shutdown)
- 기존 설계: "기본 SPSC, 필요 시 MPSC"

### 대안 분석

**A. 코어 쌍마다 SPSC 큐**
- N코어 = N×(N-1)개 큐 (8코어 = 56개)
- 경합 제로, 최고 성능
- 단점: 관리 복잡도 높음, 외부 소스(Kafka, 메인 스레드)가 보내려면 별도 처리 필요

**B. 코어당 MPSC 수신 큐 1개**
- N코어 = N개 큐 (8코어 = 8개)
- 여러 소스가 한 코어의 큐에 동시에 enqueue 가능
- 모든 시나리오(1:1, 1:N, 외부→코어)를 단일 구조로 커버

### 결정: B (코어당 MPSC 1개)

### 근거
1. 코어 간 통신은 핫패스(메시지 수신/파싱)보다 빈도가 훨씬 낮음
2. MPSC 락프리 큐 성능이 이 빈도에서 충분
3. N×(N-1) vs N — 관리 단순성이 압도적
4. 브로드캐스트, Kafka 라우팅, 관리 명령 모두 동일한 구조로 처리

---

## ADR-11: 에러 전파 경로

### 맥락
- 핸들러에서 에러 발생 시 클라이언트에 어떻게 전달하는가
- 에러 핸들링 전략(expected/예외/abort)은 확정, 계층 간 전파 방식 미정

### 대안 분석

**A. 프레임워크 자동 ErrorResponse**
- 핸들러가 에러 리턴 → 프레임워크가 자동으로 ErrorResponse 생성/전송
- 단점: 커스텀 에러 메시지 어려움

**B. 서비스 개발자 직접 구성**
- 매번 에러 응답 코드를 직접 작성
- 완전한 제어권, 하지만 반복 코드 + 실수로 에러 응답 누락 가능

**C. 기본 자동 + 오버라이드 가능**
- `co_return apex::error(ErrorCode::X)` → 자동 ErrorResponse (90% 케이스)
- 커스텀 필요 시 `session.send()` 후 `co_return apex::ok()` (10% 케이스)
- 핸들러가 에러 리턴했는데 응답을 안 보냈으면 자동 ErrorResponse 생성 (안전망)

### 결정: C (기본 자동 + 오버라이드)

### 근거
1. 90% 케이스를 한 줄로 처리 — 개발 생산성
2. 10% 커스텀 케이스도 자유롭게 대응 가능
3. "에러 리턴했는데 응답 안 보냄" 안전망으로 버그 방지
4. 기존 경험(방식 B)의 불편함을 프레임워크 레벨에서 해결

---

## ADR-12: 분산 추적 (trace_id)

### 맥락
- MSA 환경에서 하나의 요청이 여러 서비스를 관통
- 단일 서버 로그 디버깅 경험은 있으나, 분산 환경 로그 추적 경험 없음
- OpenTelemetry 풀스택은 복잡도 과다

### 결정: trace_id 경량 자체 구현

### 근거
1. Gateway에서 trace_id 생성 → 세션 컨텍스트 자동 주입 → Kafka 헤더 전파 → 로그 자동 첨부
2. 서비스 개발자가 trace_id를 의식할 필요 없음 (프레임워크가 자동 처리)
3. Kibana에서 trace_id 검색 한 번으로 전체 흐름 파악 — 단일 서버 디버깅과 동일한 경험
4. OpenTelemetry의 80% 가치를 10% 복잡도로 획득
5. 나중에 OpenTelemetry 풀스택으로 확장 가능한 구조 유지

---

## ADR-13: 개발 편의 — docker-compose 프로파일 + 스캐폴딩

### 맥락
- 로컬 개발 시 컨테이너 7개 + 서비스 3개 = 10개 프로세스 부담
- 새 서비스 추가 시 보일러플레이트 8단계

### 결정
1. **docker-compose 프로파일 분리**: 기본(Kafka,Redis,PG — 프로파일 없이 항상 실행) / observability(+Prometheus,Grafana) / full(향후)
2. **서비스 스캐폴딩 스크립트**: apex_tools/new-service.sh로 보일러플레이트 자동 생성

### 근거
1. 평소 개발은 Kafka+Redis+PG 3개만 띄우면 충분 (로깅은 ConsoleSink)
2. 관측성 스택은 필요할 때만 추가 기동
3. 스캐폴딩으로 CMake, TOML, Dockerfile, K8s manifest 자동 생성 → 비즈니스 로직에 집중

---

## ADR-14: 구현 로드맵 — 에이전트 팀 병렬 기반

### 맥락
- Claude Opus 4.6 에이전트 팀 기능으로 독립 작업 병렬 수행 가능
- 컨텍스트 압축 반복 시 품질 저하 우려 → 세션 단위 분리 필요

### 결정: 병렬 구현 → 단일 통합 → 태그 사이클

### 근거
1. 1세션 = 파일 1~3개 + 테스트로 컨텍스트 크기 제한 → 품질 유지
2. v0.1.0.0에서 헤더 인터페이스 사전 정의 → 병렬 작업의 "계약"
3. .5 통합 세션이 품질 체크포인트 역할
4. 각 세션 종료 시 docs/apex_common/progress/ 체크포인트 → 다음 세션에서 컨텍스트 복구
5. Phase당 3~4 에이전트 병렬 → 순차 대비 약 3배 속도 향상

---

## ADR-15: I/O 백엔드 — epoll 기본, io_uring 선택

### 맥락
- 포트폴리오 원본에서 "io_uring 백엔드 자동 활성화" 명시
- Boost.Asio 1.78+에서 BOOST_ASIO_HAS_IO_URING 매크로로 지원
- K8s/Docker 환경에서 배포 예정

### 대안 분석

**A. io_uring 기본 활성화**
- 최고 성능 추구에 부합
- 단점: Docker 기본 seccomp가 io_uring 시스콜 차단, 컨테이너 호환성 문제

**B. epoll 기본, io_uring 선택적**
- 안정적, 컨테이너 완벽 호환
- CMake 옵션으로 io_uring 빌드 가능

### 결정: B (epoll 기본, io_uring 선택적)

### 근거
1. 네트워크 I/O에서 io_uring은 epoll 대비 10~20% 개선에 그침 (극적 차이 아님)
2. io_uring의 진짜 강점은 파일 I/O — 우리 프레임워크는 네트워크 I/O 중심
3. Docker 기본 seccomp 프로파일이 io_uring 차단 → 프로덕션 배포에서 마찰
4. Google조차 프로덕션에서 io_uring 비활성화한 이력 (커널 보안 취약점)
5. BOOST_ASIO_HAS_IO_URING은 컴파일 타임 스위치 → 런타임 전환 불가, 별도 빌드 필요
6. "io_uring의 한계를 알면서 선택하지 않은" 판단이 포트폴리오에서 깊이를 보여줌

---

## ADR-16: 코루틴 + 세션 수명 안전

### 맥락
- C++20 코루틴에서 co_await 중 세션 객체가 파괴되면 use-after-free
- C++ 비동기 코드의 가장 흔한 크래시 원인
- 서비스 개발자에게 수명 관리를 맡기면 100% 버그 발생

### 결정: 프레임워크가 intrusive_ptr로 세션 수명 강제 관리

### 근거
1. 코루틴이 intrusive_ptr<Session>을 캡처 → co_await 중에도 세션 유지
2. 이미 끊긴 세션에 send 시 graceful 무시 (크래시 방지)
3. 서비스 개발자가 수명을 의식할 필요 없음 — 프레임워크의 존재 이유
4. intrusive_ptr + co_spawn 패턴으로 구현 (non-atomic refcount, per-core 단일 스레드 보장)

---

## ADR-17: 프로토콜 버전 관리

### 맥락
- FlatBuffers 스키마 업데이트 시 구버전 클라이언트 호환성 필요
- msg_id 변경 시 디스패치 깨짐

### 결정: 고정 헤더에 version(uint8) 필드 추가

### 근거
1. FlatBuffers 필드 추가는 하위 호환 (새 필드는 기본값)
2. 비호환 변경 시 ver 올리고 Gateway가 구버전에 업데이트 응답
3. 헤더 1바이트 추가 비용으로 미래 호환성 확보
4. uint8 = 256 버전, 충분한 여유

---

## ADR-18: 백프레셔

### 맥락
- 서비스가 유입 트래픽을 못 따라가면 MPSC 큐에 메시지가 무한 축적 → OOM
- Kafka는 디스크 기반이라 자연스러운 버퍼이지만, 인메모리 큐는 아님

### 결정: MPSC 큐 용량 제한 + 백프레셔 전파

### 근거
1. max_capacity 초과 시 enqueue가 expected<void, BackpressureError> 반환
2. 큐 80% 도달 시 Gateway에 슬로우다운 시그널
3. Gateway가 클라이언트에 429 Too Many Requests 응답
4. 단순한 메커니즘으로 OOM 방지 — 복잡한 flow control 불필요

---

## ADR-19: 플랫폼 분기

### 맥락
- 개발 환경: Windows 10
- 배포 환경: Linux Docker/K8s
- SO_REUSEPORT, 시그널 등 Linux 전용 기능 사용

### 결정: 플랫폼별 fallback 제공

### 근거
1. SO_REUSEPORT (Linux) → 단일 acceptor + round-robin (Windows fallback)
2. SIGTERM/SIGHUP (Linux) → Windows 등가 메커니즘
3. 동작은 동일, 성능 차이만 있으므로 개발 편의성 확보
4. 성능 테스트/배포는 Linux Docker에서 수행

---

## ADR-20: 코어 간 요청-응답 패턴 (Seastar 참고)

### 맥락
- MPSC 큐가 fire-and-forget만 지원하면 코어 간 상호작용이 불편
- Seastar는 smp::submit_to(core, lambda) → future<T>로 해결
- 실제 시나리오: "코어 2야, 세션 #5678 상태 알려줘"

### 결정: MPSC 위에 요청 ID + 응답 콜백 래퍼

### 근거
1. `cross_core_call(target_core, lambda) → awaitable<T>` 형태로 제공
2. 내부: 요청 ID 발번 → 대상 코어 MPSC에 enqueue → 응답 대기 (코루틴 suspend)
3. 대상 코어가 처리 후 요청 코어의 MPSC에 응답 enqueue → resume
4. fire-and-forget도 여전히 지원 (응답 불필요 시)
5. Seastar에서 검증된 패턴을 우리 아키텍처에 적용

---

## ADR-21: 코루틴 프레임 할당 전략

### 맥락
- C++20 코루틴은 프레임마다 힙 할당 (수백 바이트~수 KB)
- 초당 수십만 메시지 = 초당 수십만 malloc
- 성능 철학 "핫패스에서 malloc 제거"에 위배

### 대안 분석

**A. promise_type에 커스텀 allocator (풀 할당)**
- operator new/delete 오버로드 → 코어별 슬랩 풀에서 할당
- malloc 완전 제거

**B. HALO (컴파일러 최적화에 의존)**
- 컴파일러가 코루틴 수명을 분석해서 힙 할당 제거
- 보장 불가, 컴파일러마다 다름

**C. 고성능 범용 allocator (mimalloc/jemalloc) + HALO 활용**
- 현대 malloc은 충분히 빠름
- HALO 최적화로 많은 경우 힙 할당 자체가 제거됨
- 병목이 확인되면 커스텀 코루틴 타입 도입 검토

### 결정: C (고성능 범용 allocator + HALO)

### 근거 (v0.2.2 갱신)
업계 주요 코루틴 프레임워크 조사 결과:
- **Seastar**: malloc 자체를 per-core allocator로 교체. promise_type 오버로드 없음
- **folly::coro**: 커스텀 코루틴 프레임 할당 없음. jemalloc 의존
- **Boost.Asio**: awaitable<T> 내부 promise_type 수정 불가. HALO 최적화 의존
- **cppcoro**: HALO 최적화를 유도하는 설계. 커스텀 풀 할당 없음

**결론**: promise_type::operator new 풀 오버로드는 업계 미검증 접근.
고성능 범용 allocator(mimalloc 또는 jemalloc) 링크 + HALO 컴파일러 최적화 활용.
벤치마크에서 코루틴 프레임 할당이 병목으로 확인될 경우, 커스텀 코루틴 타입 도입 검토.

### 기존 A안 보류 사유
1. Boost.Asio awaitable<T>의 promise_type은 프레임워크 외부에 있어 오버로드 불가
2. 커스텀 코루틴 타입을 만들면 Boost.Asio의 co_spawn/use_awaitable 생태계와 분리됨
3. 위 4개 프레임워크 모두 promise_type 풀 할당을 채택하지 않음 — 실전 미검증

---

## ADR-22: Zero-copy 범위 명시

### 맥락
- 설계 문서에서 "zero-copy"를 여러 번 강조
- 실제로 링 버퍼 wrap-around 시 데이터가 불연속 → FlatBuffers 직접 접근 불가

### 결정: "실질적 zero-copy" — wrap 시 예외 copy 허용

### 근거
1. 대부분(99%+) 메시지는 링 버퍼의 연속 영역에 위치 → 진짜 zero-copy
2. wrap-around에 걸리는 극소수 메시지만 연속 버퍼에 copy
3. 완전한 zero-copy를 위한 대안(mmap 트릭, 더블 버퍼)은 복잡도 대비 이점이 미미
4. 설계 문서에 정직하게 범위를 명시 — 포트폴리오에서 신뢰도 상승

---

## ADR-23: 외부 라이브러리 스레드 모델과 shared-nothing 범위

### 맥락
- librdkafka, spdlog, prometheus-cpp이 각각 자체 스레드 생성
- "shared-nothing"이라고 했지만 실제로는 외부 스레드가 존재

### 결정: shared-nothing 범위를 비즈니스 로직 코어로 한정

### 근거
1. 외부 라이브러리의 내부 스레딩은 통제 불가 (librdkafka가 대표적)
2. 이 스레드들은 프레임워크 코어와 MPSC 큐 또는 fd 등록으로만 소통
3. 비즈니스 로직이 실행되는 코어 간에는 완전한 shared-nothing 유지
4. 정직한 범위 명시가 "shared-nothing을 이해하고 있다"는 증거
