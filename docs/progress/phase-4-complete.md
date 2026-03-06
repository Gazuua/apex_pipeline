# Phase 4 Complete

## 구현된 컴포넌트

### Agent A: FlatBuffers 코드젠 + route<T> 타입 디스패치
1. **FlatBuffers 스키마 3개** - echo.fbs, error_response.fbs, heartbeat.fbs
2. **CMake flatc 코드젠 파이프라인** - 스키마 자동 컴파일, generated/ 헤더 생성
3. **route<T>()** - ServiceBase에 FlatBuffers 타입 안전 디스패치 추가
   - 핸들러 호출 전 FlatBuffers Verifier 자동 검증
   - 잘못된 버퍼는 크래시 없이 무시
   - 기존 handle() 로우 핸들러와 공존

### Agent B: 세션 관리
4. **Session** - TCP 소켓 + RingBuffer 래핑, shared_ptr 기반 수명 관리
   - 상태 머신: Connected -> Active -> Closing -> Closed
   - send(WireHeader, payload) / send_raw() / close()
   - 닫힌 세션에 send 시 graceful하게 false 반환
5. **SessionManager** - 코어별 세션 관리 (NOT thread-safe, single-core 전용)
   - create/find/remove + SessionId 기반 추적
   - TimingWheel 연동 하트비트 타임아웃
   - touch_session()으로 활동 시 타임아웃 리셋
   - 타임아웃 콜백으로 커스텀 처리

### Agent C: 에러 전파
6. **ErrorCode** - 프레임워크 에러(1-999) + 애플리케이션 에러(1000+) 열거형
7. **Result<T>** - std::expected<T, ErrorCode> 타입 별칭 + ok()/error() 헬퍼
8. **ErrorSender** - FlatBuffers ErrorResponse 페이로드 + wire_flags::ERROR_RESPONSE 프레임 빌더

## 신규 파일
- `core/include/apex/core/error_code.hpp`
- `core/include/apex/core/result.hpp`
- `core/include/apex/core/session.hpp`
- `core/include/apex/core/session_manager.hpp`
- `core/include/apex/core/error_sender.hpp`
- `core/src/session.cpp`
- `core/src/session_manager.cpp`
- `core/src/error_sender.cpp`
- `core/schemas/echo.fbs`
- `core/schemas/error_response.fbs`
- `core/schemas/heartbeat.fbs`

## 수정된 파일
- `core/include/apex/core/service_base.hpp` (route<T> 추가)
- `core/CMakeLists.txt` (FlatBuffers 코드젠 + 신규 소스)
- `core/tests/unit/CMakeLists.txt` (신규 테스트 4개)

## 테스트 현황
- Phase 4 신규: 20개 테스트 (4 스위트)
  - FlatBuffersDispatch: 3개 (RouteTypedMessage, RouteInvalidFlatBuffer, RouteAndRawHandlerCoexist)
  - Session: 4개 (InitialState, SendFrame, SendAfterClose, RecvBufferAccessible)
  - SessionManager: 6개 (CreateAndFindSession, RemoveSession, HeartbeatTimeout, TouchResetsTimeout, MultipleSessions, DisabledHeartbeat)
  - ErrorPropagation: 7개 (OkValue, ErrorValue, VoidOk, VoidError, NameLookup, BuildErrorFrame, BuildErrorFrameNoMessage)
- 전체 누적: 89개 테스트 (Phase 2: 29 + Phase 3: 40 + Phase 4: 20)

## 발견된 이슈 및 해결
- HeartbeatTimeout 테스트: TimingWheel이 deadline 이후 다음 tick에서 fire하는 동작에 맞춰 tick 횟수 조정 (3→4)
- TouchResetsTimeout 테스트: touch 후 새 deadline까지 추가 tick 필요 (Plan 대비 tick 수 조정)
- FlatBuffers Verifier 실패 시 dispatch 반환값: 핸들러 자체는 호출됐으나 내부에서 skip하므로 has_value() == true

## Phase 2 + 3 컴포넌트 활용
- RingBuffer -> Session 수신 버퍼
- TimingWheel -> SessionManager 하트비트 타임아웃
- WireHeader -> Session::send() 프레임 직렬화
- FrameCodec -> (Phase 4.5 Server에서 활용 예정)
- MessageDispatcher -> ServiceBase route<T>() 내부 핸들러 등록

## 다음: Phase 4.5 (통합)
- Server 클래스 (TcpAcceptor + SessionManager + MessageDispatcher 통합)
- 핸들러 시그니처에 SessionPtr 추가
- E2E 통합 테스트 7개 + 채팅 예제
- v0.2.0 태그
