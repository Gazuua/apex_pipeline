# FSD Backlog — 백로그 소탕 자동화 시스템 설계

**BACKLOG-123** | 2026-03-21

---

## 개요

슬래시 커맨드 `/fsd-backlog` 한 번으로 백로그를 스캔하고, 설계 결정 없이 자동 해결 가능한 항목을 선별하여, 착수부터 머지까지 완전 자율 수행하는 시스템.

auto-review가 지속적으로 발견하는 버그성 이슈를 깔끔하게 소탕하여, 설계 결정이 필요한 로드맵 작업에 집중할 수 있게 하는 것이 목적이다.

## 구현 방식

기존 `apex_tools/claude-plugin/`에 슬래시 커맨드 스킬(`commands/fsd-backlog.md`)을 추가한다. 스킬 프롬프트가 소탕 고유의 행동만 지시하고, 나머지는 기존 CLAUDE.md 워크플로우를 그대로 따른다.

## 전체 흐름

```
/fsd-backlog
    │
    ├─ Phase 1: 스캔 & 선별
    │   ├─ BACKLOG.md 파싱 (NOW → IN VIEW → DEFERRED 순)
    │   ├─ 각 항목의 코드베이스 현황 분석
    │   ├─ 자동화 가능 번들 확정
    │   └─ 번들 0개면 다음 섹션으로 계속 → 끝까지 없으면 종료
    │
    ├─ Phase 2: 착수
    │   ├─ branch-handoff.sh status → 점유 현황 확인
    │   ├─ git checkout -b feature/fsd-backlog-<타임스탬프> (최신 main)
    │   └─ branch-handoff.sh notify start --skip-design
    │
    ├─ Phase 3: 구현
    │   ├─ 항목별 수정 (드롭 시 백로그 설명란에 사유 기록)
    │   ├─ clang-format
    │   └─ queue-lock.sh build debug
    │
    ├─ Phase 4: 이후 기존 워크플로우
    │   └─ 리뷰·문서·머지 전부 기존 지침 + 자율 판단
    │
    └─ 결과: 소탕 완료 리포트 (터미널 출력)
```

## Phase 1: 스캔 & 선별 로직

### 스캔 순서

시간축 우선순위: NOW → IN VIEW → DEFERRED. 현재 섹션에서 자동화 가능 항목이 없으면 다음 섹션으로 넘어간다. 모든 섹션을 확인했는데 자동화 가능 항목이 없으면 브랜치 생성 없이 분석 결과만 리포트하고 종료.

### 동적 판별 기준

에이전트가 각 항목에 대해 순차 평가:

1. **다른 에이전트가 점유 중인가?** → YES → 스킵
2. **타입이 자동화에 적합한가?**
   - bug, design-debt, test, docs, infra → 후보
   - security, perf → 코드 분석 후 판단
3. **실제 코드를 확인했을 때 수정 방향이 명확한가?**
   - 가이드(apex_core_guide.md, CLAUDE.md)에 답이 있는가?
   - 기존 코드 패턴을 따르면 되는가?
   - YES → 번들에 포함
   - NO (설계 결정 필요) → 백로그 설명란에 분석 결과 메모, 스킵

### 번들 구성

- 선별된 항목들을 스코프 연관성 + 파일 겹침 기준으로 자연스럽게 묶음
- 번들 크기는 에이전트 자율 판단
- **번들 확정 = Phase 1 종료 = Phase 2 진입 조건** (브랜치 생성 전에 완료)

## Phase 2: 착수

`branch-handoff.sh status`로 전체 점유 현황을 확인한 뒤, 번들 내 백로그가 다른 에이전트에 점유되어 있으면 해당 항목을 드롭하고 나머지로 재구성한다.

브랜치명은 `feature/fsd-backlog-<타임스탬프>`. `notify start --skip-design`으로 바로 implementing 상태에 진입한다.

## Phase 3: 구현 & 드롭 처리

기존 워크플로우대로 구현. 구현 중 자동화 범위 밖이라고 판단한 항목은 드롭한다.

### 드롭 시점

| 시점 | 상황 | 처리 |
|------|------|------|
| Phase 1 (선별) | 코드 분석 결과 설계 결정 필요 | 백로그 설명란에 분석 결과 추가, 번들에서 제외 |
| Phase 3 (구현 중) | 파보니까 예상보다 복잡 | 이미 한 수정이 있으면 revert, 백로그 설명란에 사유 추가 |

### 백로그 메모 형식

기존 설명 필드 끝에 `[FSD 분석]` 태그로 추가:

```markdown
### #27. FrameError→ErrorCode 매핑 구분 불가
- **등급**: MINOR
- **스코프**: core
- **타입**: design-debt
- **설명**: 모두 ErrorCode::InvalidMessage로 매핑. 세분화 검토.
  **[FSD 분석 2026-03-21]** ErrorCode enum 확장이 필요하며,
  클라이언트 측 에러 핸들링 호환성 검토가 선행되어야 함.
```

다음 실행 시 이 태그가 있으면 "이미 분석했고 자동화 불가 판정을 받은 항목"이라는 힌트가 된다.

## 스킬 프롬프트 구조

파일: `apex_tools/claude-plugin/commands/fsd-backlog.md`

프롬프트가 담당하는 건 두 가지:

### 1. 소탕 워크플로우 지시

기존 CLAUDE.md 지침에 없는 이 커맨드 고유의 행동만 정의:

- BACKLOG.md 스캔 → 동적 판별 → 번들 확정
- 자동화 불가 항목은 백로그 설명란에 `[FSD 분석]` 태그로 분석 사유 메모 후 드롭
- 번들 0개면 다음 섹션 스캔 계속, 끝까지 없으면 브랜치 생성 없이 종료
- 번들 확정 후 기존 워크플로우 진입 (착수→구현→검증→리뷰→문서→머지 전부 기존 지침 따름)
- 브랜치명: `feature/fsd-backlog-<타임스탬프>`
- 커밋 메시지: `fix(스코프): BACKLOG-N,M,... 백로그 소탕` 형태

### 2. 핸드오프 사용 가이드

에이전트가 헤매지 않도록 정확한 명령어 순서를 포함:

```bash
# 1. 점유 현황 확인
branch-handoff.sh status

# 2. 브랜치 생성 (최신 main 기반, enforce-rebase hook이 자동 보장)
git checkout -b feature/fsd-backlog-<타임스탬프>

# 3. 착수 선언 (--summary 필수, 비우면 hook이 차단)
branch-handoff.sh notify start \
  --skip-design \
  --backlogs "BACKLOG-N,M" \
  --scopes "core,shared" \
  --summary "BACKLOG-N,M 백로그 소탕"

# 4. (구현 ~ 머지까지 기존 워크플로우)

# 5. 머지 완료 + 점유 릴리즈
branch-handoff.sh notify merge \
  --summary "BACKLOG-N,M 소탕 완료"
```

### 프롬프트가 하지 않는 것

- 빌드 방법, clang-format, auto-review 절차, 문서 규칙 등 → 전부 기존 CLAUDE.md에 있으므로 중복 기술하지 않음
- 번들 크기, 리뷰 여부 등 → 자율 판단 영역

## 결과 리포트

머지까지 완료되면 터미널에 소탕 결과를 출력:

```
═══════════════════════════════════════
  FSD Backlog Sweep — 완료
═══════════════════════════════════════

  브랜치: feature/fsd-backlog-20260321
  PR: #87 (squash merged)

  ✅ 해결: BACKLOG-26, BACKLOG-29, BACKLOG-63
  ⏭️ 드롭: BACKLOG-27 (ErrorCode enum 확장 필요, 설명란 메모 완료)
           BACKLOG-100 (보안 정책 설계 결정 필요, 설명란 메모 완료)

═══════════════════════════════════════
```

## 산출물

| 파일 | 설명 |
|------|------|
| `apex_tools/claude-plugin/commands/fsd-backlog.md` | 슬래시 커맨드 스킬 프롬프트 |
| `apex_tools/claude-plugin/.claude-plugin/plugin.json` | 커맨드 등록 (기존 파일 수정) |
