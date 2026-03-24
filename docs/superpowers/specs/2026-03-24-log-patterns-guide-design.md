# 로그 패턴 가이드 설계 스펙

**일자**: 2026-03-24
**상태**: 승인됨
**범위**: 문서 신규 작성 + CLAUDE.md 지침 연동

---

## 1. 배경 및 목적

v0.5.11.0에서 ScopedLogger 로깅 인프라가 완성되었으나, 실제 서비스 기동 시 찍히는 로그 패턴에 대한 가이드 문서가 부재하다.

에이전트(auto-review, 디버깅 서브에이전트)가 버그 추적 시 로그를 분석하려면 정상/비정상 패턴의 판별 기준이 필요하고, 사람이 직접 로그를 볼 때도 동일한 기준이 유용하다.

**목표**: 에이전트와 사람 모두가 활용할 수 있는 로그 패턴 가이드를 작성하고, 에이전트가 적절한 시점에 참조하도록 CLAUDE.md에 지침을 연동한다.

## 2. 산출물

| # | 항목 | 설명 |
|---|------|------|
| 1 | `docs/apex_core/log_patterns_guide.md` | 로그 패턴 가이드 (에이전트+사람 겸용) |
| 2 | `CLAUDE.md` 포인터 테이블 1행 추가 | 로그 패턴 가이드 위치 안내 |
| 3 | `CLAUDE.md` 에이전트 작업 섹션 1항 추가 | 버그/장애 추적 시 필독 지침 |
| 4 | `CLAUDE.md` 머지 전 필수 갱신 목록 1항 추가 | `log_patterns_guide.md`(로깅 영역 변경 시) |

## 3. 가이드 문서 구조

### 3.1 로그 아키텍처 개요

- 이중 로거 체계: `"apex"` (프레임워크) / `"app"` (서비스 비즈니스 로직)
- 각 로거의 독립 레벨 제어 (`framework_level` vs `level`)
- ScopedLogger 태그 조립 구조: `[file:line Func] [core=N][Component][sess=...][msg=0x...][trace=...] message`
- 출력 대상: 콘솔(텍스트 패턴) + 파일(레벨별 6파일 분리, JSON 포맷 선택 가능)
- spdlog 콘솔 패턴: `%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v`

### 3.2 태그 레퍼런스

각 태그의 의미, 출현 조건, 포맷을 정리:

| 태그 | 의미 | 출현 조건 |
|------|------|-----------|
| `[file:line Func]` | source_location (파일명만, 경로 제거) | 항상 |
| `[core=N]` | 코어 ID | `core_id != NO_CORE`일 때 |
| `[Component]` | 컴포넌트명 (예: Server, SessionManager) | 항상 |
| `[sess=N]` | 세션 ID (코어별 단조 증가 uint64 정수) | SessionId 오버로드 사용 시 |
| `[msg=0xHHHH]` | 메시지 ID (프로토콜) | msg_id 오버로드 사용 시 |
| `[trace=hex]` | 요청 추적 correlation ID | `with_trace()` 사용 시 |

### 3.3 정상 패턴 카탈로그

실제 코드에서 추출한 예시 로그와 함께 정리:

- **기동 시퀀스**: config 로드 → adapters init → Phase 1/2/3 → listener bind → ready
- **세션 생명주기**: session created → enqueue_write → (timeout 또는 정상 close)
- **메시지 디스패치**: frame decode → dispatch_message → 서비스 핸들러
- **주기적 태스크**: schedule → execute → stop_all
- **셧다운 시퀀스**: shutdown initiated → sessions drained → adapters closed → core engine stopped

### 3.4 비정상 패턴 카탈로그

레벨별로 분류하되, 각 패턴에 대해:
- **로그 예시**: 실제 찍히는 형태
- **의미**: 왜 이 로그가 찍히는지
- **확인할 코드**: 소스 파일:라인 범위
- **조치 방향**: 무엇을 확인/수정해야 하는지

**warn 레벨** (주의 — 즉시 장애는 아니지만 조치 필요):
- `session SlabAllocator exhausted, heap fallback` — 슬랩 풀 고갈, 세션 급증 의심
- `enqueue_write queue full` — 쓰기 큐 포화, 클라이언트 수신 지연 또는 과부하
- `double-free detected` — 슬랩 할당기 이중 해제, 수명 관리 버그
- `write overflow` — 링 버퍼 쓰기 초과
- `dispatch ... op=N — no handler` (CrossCoreDispatcher) — 등록되지 않은 cross-core op 코드
- `close id=N socket error: ...` — 소켓 종료 시 에러 (보통 이미 닫힌 소켓)
- `state CLOSED->OPEN failures=N/M` (CircuitBreaker) — 연속 실패로 서킷 오픈
- `state HALF_OPEN->OPEN (probe failed)` (CircuitBreaker) — 반개방 프로브 실패, 서킷 재오픈

**error 레벨** (오류 — 기능 장애 발생):
- `cross-core task exception` — 코어 간 태스크 실행 중 예외
- `timeout callback exception` — 세션 타임아웃 콜백 예외

**critical 레벨** (치명적 — 서비스 기동/운영 불가):
- `invalid bind_address` — 리스너 바인드 실패, 서비스 시작 불가

### 3.5 트러블슈팅 체크리스트

증상 기반 진단 플로우:

- **서비스가 기동되지 않음** → `critical` 로그 grep → bind 실패 / config 로드 실패 확인
- **세션이 예기치 않게 끊김** → `[sess=...]` + warn/error grep → timeout / write error / queue full 확인
- **메시지가 처리되지 않음** → `dispatch` + `no handler` grep → 핸들러 등록 누락 확인
- **메모리 사용량 증가** → `heap fallback` + `double-free` grep → 슬랩 풀 / 수명 관리 확인
- **외부 서비스 연결 불안정** → `CircuitBreaker` grep → failure count 추이 + 상태 전이 확인

### 3.6 컴포넌트 맵

전체 컴포넌트 목록을 표로 정리. 실제 코드에서 추출:

| 컴포넌트 | 로거 | 코어 바인딩 | 소속 |
|----------|------|------------|------|
| Server | apex | NO_CORE | apex_core |
| CoreEngine | apex | per-core | apex_core |
| SessionManager | apex | per-core | apex_core |
| TcpAcceptor | apex | NO_CORE | apex_core |
| Listener | apex | NO_CORE | apex_core |
| ... | ... | ... | ... |
| GatewayPipeline | app | NO_CORE | gateway |
| KafkaProducer | app | NO_CORE | apex_shared |
| RedisConnection | app | NO_CORE | apex_shared |
| ... | ... | ... | ... |

(구현 시 코드에서 전체 목록 추출)

## 4. CLAUDE.md 변경

### 4.1 가이드 포인터 테이블

`상세 가이드 포인터` 테이블에 1행 추가:

```
| 로그 패턴 가이드 (정상/비정상 패턴, 트러블슈팅) | `docs/apex_core/log_patterns_guide.md` |
```

### 4.2 에이전트 작업 섹션

`에이전트 작업` 섹션 마지막 항목 뒤에 추가:

```
- **버그/장애 추적 시 로그 패턴 가이드 필독** — 로그 분석이 수반되는 디버깅 작업에서는 `docs/apex_core/log_patterns_guide.md`를 사전 참조. 정상/비정상 패턴 판별 기준과 컴포넌트별 확인 포인트가 정리되어 있음
```

### 4.3 머지 전 필수 갱신 목록

기존 쉼표 구분 인라인 목록의 마지막 항목 뒤에 동일 형태로 추가:

```
, `docs/apex_core/log_patterns_guide.md`(로깅 영역 변경 시)
```

## 5. 설계 결정

- **톤**: 에이전트+사람 겸용 (B안). 패턴의 의미와 맥락을 함께 설명하여 에이전트의 정확한 판단과 사람의 이해를 동시에 지원
- **가이드 위치**: `docs/apex_core/` 하위. 코어 프레임워크 가이드(`apex_core_guide.md`)와 동일 디렉토리에 배치하여 코어 문서의 일관성 유지
- **CLAUDE.md 연동**: 포인터 + 트리거 조건("버그/장애 추적 시") 분리. 평시에는 컨텍스트 부담 없이, 필요 시에만 로드
- **머지 전 갱신**: 조건부 (`로깅 영역 변경 시`). 무관한 작업에 불필요한 갱신 의무를 부과하지 않음
- **문서 헤더**: 기존 `apex_core_guide.md`와 동일한 버전/갱신일 헤더 형식 적용 (`**버전**: vX.Y.Z.W | **최종 갱신**: YYYY-MM-DD`)
