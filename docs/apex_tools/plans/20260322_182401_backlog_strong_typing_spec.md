# 백로그 강타입 + 핸드오프 연동 설계 스펙

- **상태**: 설계 확정
- **백로그**: BACKLOG-126 (강화 Phase 내 추가 작업)
- **스코프**: TOOLS
- **작성일**: 2026-03-22
- **선행**: 강화 Phase (Config + Logging + E2E) 구현 완료

---

## 1. 목표

백로그 시스템의 모든 열거형 필드에 코드 레벨 타입 강제를 적용하고,
핸드오프 시스템과의 연동을 체계화하여 백로그 작업 추적의 정합성을 보장한다.

---

## 2. 열거형 정의 (UPPER_SNAKE_CASE)

모든 열거형은 Go const로 정의하고, `Manager.Add()` / `Manager.Resolve()` 진입부에서 검증한다.
잘못된 값 → 즉시 에러 반환.

| 필드 | 허용값 |
|------|--------|
| **Severity** | `CRITICAL`, `MAJOR`, `MINOR` |
| **Timeframe** | `NOW`, `IN_VIEW`, `DEFERRED`, `""` (히스토리 import용) |
| **Type** | `BUG`, `DESIGN_DEBT`, `DOCS`, `INFRA`, `PERF`, `SECURITY`, `TEST` |
| **Status** | `OPEN`, `FIXING`, `RESOLVED` |
| **Resolution** | `FIXED`, `DOCUMENTED`, `WONTFIX`, `DUPLICATE`, `SUPERSEDED` |
| **Scope** | `CORE`, `SHARED`, `GATEWAY`, `AUTH_SVC`, `CHAT_SVC`, `INFRA`, `CI`, `DOCS`, `TOOLS` |

### Scope 특수 처리

Scope는 복수값(`"CORE, SHARED"`)이 가능하다.
검증 시 쉼표+공백 분리 후 각 토큰을 개별 검증한다.

### Status 전이

전이 제한 없음 — 유효한 enum 값이면 어떤 상태에서든 전이 허용.
`RESOLVED → OPEN` (재오픈), `FIXING → OPEN` (착수 포기) 등 모두 가능.

---

## 3. Handoff notify start 분리

### 백로그 작업

```bash
handoff notify start --backlog 126,132 --summary "..." --scopes CORE,SHARED [--skip-design]
```

- `--backlog` 필수 (`IntSliceVar`). 누락 또는 빈 값/0 → 차단 + 에러 메시지
- 복수 백로그 지원: `--backlog 126,132` 또는 `--backlog 126 --backlog 132`
- 각 백로그 항목을 `FIXING`으로 전이 (이미 `FIXING`이면 중복 착수 에러)
- 중복 착수 차단은 `branch_backlogs` UNIQUE 제약 + FIXING 상태 체크 이중 보호

### 비백로그 작업

```bash
handoff notify start job --summary "CI 파이프라인 정비" --scopes CI [--skip-design]
```

- `job` 서브커맨드로 명시적 구분
- 백로그 연결 없음. `branch_backlogs` 미사용
- branches + notifications 테이블에만 등록

---

## 4. DB 변경

### 신규 테이블: `branch_backlogs` (junction)

```sql
CREATE TABLE branch_backlogs (
    branch     TEXT    NOT NULL,
    backlog_id INTEGER NOT NULL,
    PRIMARY KEY (branch, backlog_id),
    FOREIGN KEY (branch) REFERENCES branches(branch),
    FOREIGN KEY (backlog_id) REFERENCES backlog_items(id)
);
```

### `branches` 테이블 변경

- `backlog_id` 컬럼 제거 → junction 테이블로 대체

### 마이그레이션

**backlog 모듈 — v3**: 기존 데이터 UPPER_SNAKE_CASE 정규화 + 스키마 기본값 갱신

| 필드 | 변환 예시 |
|------|-----------|
| type | `bug` → `BUG`, `design-debt` → `DESIGN_DEBT` |
| status | `open` → `OPEN`, `resolved` → `RESOLVED` |
| scope | `core, shared` → `CORE, SHARED`, `auth-svc` → `AUTH_SVC` |
| resolution | 오염 데이터 정리: `WONTFIX → **정정...` → `FIXED` 등 패턴 매칭으로 수정 |

스키마 기본값도 갱신: `DEFAULT 'open'` → `DEFAULT 'OPEN'` (테이블 재생성으로 반영).

scope 변환 규칙: 쉼표+공백 분리 → 각 토큰 `UPPER()` + `-` → `_` 치환 → 재결합.

**handoff 모듈 — v2**: junction 테이블 도입

1. `branch_backlogs` 테이블 생성
2. 기존 `branches.backlog_id` 데이터를 junction으로 이관 (NULL 값 스킵)
3. `branches` 테이블 재생성 (backlog_id 컬럼 제거)
4. 전체를 트랜잭션으로 감싸 원자적 실행

마이그레이션 실행 순서: migrator가 모듈명 알파벳순 → `backlog` v3이 `handoff` v2보다 먼저 실행 (정규화 선행 보장).

---

## 5. 신규 기능: backlog release

```bash
apex-agent backlog release --id 126 --reason "스코프 축소로 이번 PR에서 제외"
```

- `--reason` 필수 — 미해결 사유 기록
- `branch_backlogs`에서 해당 backlog_id의 연결 제거
- 백로그 status가 `FIXING`이면 → `OPEN`으로 전이
- 다른 status (`OPEN`, `RESOLVED`)이면 status는 변경 없이 junction 제거만 수행
- 백로그 `description`에 release 이력 append

IPC 라우트: `backlog.release`
파라미터: `{"id": int, "reason": string, "branch": string}`

---

## 6. 연동 로직 상세

### 모듈 간 의존성

핸드오프 → 백로그 단방향 의존:

- `handoff.Manager`가 `backlog.Manager`를 참조 (생성자 주입)
- `NotifyStart`에서 `backlog.Manager.SetStatus(id, FIXING)` 호출
- `branch_backlogs` 테이블은 handoff 모듈이 소유 (handoff 스키마 내 생성)
- `backlog.Release()`는 `branch_backlogs`에 직접 SQL 접근 (같은 DB 내 cross-table 쿼리)

### NotifyStart 시그니처 변경

```go
// 기존
func (m *Manager) NotifyStart(branch, workspace, summary string, backlogID int, scopes string, skipDesign bool) (int, error)

// 변경
func (m *Manager) NotifyStart(branch, workspace, summary string, backlogIDs []int, scopes string, skipDesign bool) (int, error)
```

`backlogIDs`가 빈 슬라이스면 `job` 모드 (비백로그 작업).

### notify start (백로그 작업) 흐름

```
CLI: handoff notify start --backlog 126,132 --summary "..."
  ↓
handoff.NotifyStart(backlogIDs=[126,132]):
  1. 각 backlog_id에 대해:
     a. backlog.Manager.Get(id) → 존재 확인
     b. status가 FIXING이면 → 에러 (이미 다른 브랜치가 작업 중)
  2. branches 테이블에 등록
  3. branch_backlogs에 (branch, 126), (branch, 132) INSERT
  4. backlog.Manager.SetStatus(id, FIXING) — 각 항목
  5. notifications 테이블에 알림 발행
  (전체 트랜잭션으로 감싸 원자적 실행)
```

### notify merge 시 FIXING 항목 강제 처리

`notify merge` 호출 시 해당 브랜치에 연결된 FIXING 백로그가 남아 있으면 **머지를 차단**한다.

```
CLI: handoff notify merge
  ↓
  1. branch_backlogs에서 현재 브랜치의 FIXING 항목 조회
  2. FIXING 항목이 있으면:
     → 에러: "미해결 백로그 N건 — resolve 또는 release 후 재시도"
     → 항목 목록 표시 (ID + 제목)
  3. 없으면 → 정상 진행
```

해결 방법 (에이전트가 선택):
- **완료됨** → `backlog resolve --id N --resolution FIXED`
- **미완료, 착수 포기** → `backlog release --id N --reason "이유 설명"`

### backlog release에 reason 필수

```bash
apex-agent backlog release --id 126 --reason "스코프 축소로 이번 PR에서 제외"
```

- `--reason` 필수. 왜 해결하지 못했는지 기록
- 백로그 항목의 `description`에 release 이력 append:
  `[RELEASED 2026-03-22 18:30:00] branch_02: 스코프 축소로 이번 PR에서 제외`

### backlog resolve 흐름

```
CLI: apex-agent backlog resolve --id 126 --resolution FIXED
  ↓
backlog.Resolve():
  1. Resolution enum 검증
  2. status → RESOLVED, resolution/resolved_at 기록
  3. branch_backlogs 연결은 유지 (이력 추적용)
```

### backlog release 흐름

```
CLI: apex-agent backlog release --id 126 --reason "사유"
  ↓
backlog.Release():
  1. branch_backlogs에서 해당 항목 연결 제거
  2. status가 FIXING이면 → OPEN으로 전이
  3. 다른 status면 → 변경 없음
  4. description에 release 이력 append:
     "[RELEASED 2026-03-22 18:30:00] branch_02: 사유"
```

---

## 7. 구현 파일

### 생성

| 파일 | 책임 |
|------|------|
| `internal/modules/backlog/enums.go` | const 정의 + `ValidateSeverity()`, `ValidateType()`, `ValidateScope()` 등 검증 함수 |

### 수정

| 파일 | 변경 |
|------|------|
| `internal/modules/backlog/manage.go` | Add/Resolve에 검증 호출, Release()/SetStatus() 추가, 하드코딩 status 문자열 전부 const 전환 (`"open"` → `StatusOpen`) |
| `internal/modules/backlog/module.go` | 마이그레이션 v3 (대문자 정규화 + DEFAULT 'OPEN' 스키마 재생성) + `release` 라우트 |
| `internal/modules/backlog/import.go` | import 시 대문자 변환 (`"bug"` → `"BUG"`, `"open"` → `"OPEN"` 등), 하드코딩 문자열 const 전환 |
| `internal/modules/backlog/export.go` | export 포맷에 대문자 반영, 하드코딩 문자열 const 전환 |
| `internal/modules/handoff/manager.go` | NotifyStart 시그니처 `backlogID int` → `backlogIDs []int`, backlog.Manager 주입, junction INSERT, FIXING 전이 |
| `internal/modules/handoff/module.go` | 마이그레이션 v2 (branch_backlogs 생성 + branches.backlog_id 이관 + branches 재생성), backlog.Manager 주입 연결 |
| `internal/modules/handoff/gate.go` | `BacklogCheck()` junction 테이블 기반으로 변경 + `ValidateMergeGate`에 FIXING 백로그 차단 로직 추가 |
| `internal/cli/handoff_cmd.go` | start job 서브커맨드 + `--backlog` IntSliceVar 필수화, handoff status에서 복수 backlog 표시 |
| `internal/cli/backlog_cmd.go` | `release` 커맨드 추가, `resolve` 도움말 수정 (`DEFERRED` 제거 → `DOCUMENTED`/`SUPERSEDED` 추가), list 필터 기본값 `OPEN` |
| `docs/CLAUDE.md` | 스코프/타입 태그 UPPER_SNAKE_CASE 갱신 |

### 테스트 수정

| 파일 | 변경 |
|------|------|
| `internal/modules/backlog/*_test.go` | 하드코딩 `"open"`, `"resolved"`, `"bug"` 등 → 대문자 const 전환 |
| `internal/modules/handoff/*_test.go` | `BacklogID int` → junction 기반 검증, `BacklogCheck` 테스트 갱신 |
| `e2e/backlog_test.go` | 대문자 enum 사용, release 시나리오 추가 |
| `e2e/handoff_test.go` | backlogIDs 슬라이스 사용, job 모드 테스트 추가 |

---

## 8. 참조

- 설계 스펙: `docs/apex_tools/plans/20260322_100656_apex_agent_design_spec.md`
- 강화 스펙: `docs/apex_tools/plans/20260322_150859_apex_agent_hardening_spec.md`
- 백로그 운영 규칙: `docs/CLAUDE.md` § 백로그 운영
