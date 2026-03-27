---
description: "FSD Backlog: 백로그 소탕 완전 자율 수행 (스캔→선별→구현→머지)"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent"]
---

# FSD Backlog — Full Self-Driving Backlog Sweep

## 목적

백로그를 스캔하여 설계 결정 없이 자동 해결 가능한 항목을 선별하고, 착수부터 머지까지 완전 자율 수행한다.

> **자동화 원칙**: 전 과정을 유저 확인 없이 자동 진행한다.
> 고칠 수 있으면 고친다. 설계 결정이 필요하면 백로그에 분석 메모를 남기고 넘어간다.

---

## Phase 1: 스캔 & 선별

`docs/BACKLOG.md`를 읽고, 각 항목을 **NOW → IN VIEW → DEFERRED** 순서로 평가한다.

### 판별 기준

각 항목에 대해 순차 평가:

1. **`[FSD 분석]` 태그가 이미 있는가?** → YES → 즉시 스킵 (상황 변화가 명확한 경우에만 재분석)
2. **다른 에이전트가 점유 중인가?** → `apex-agent handoff status`의 "Backlog items in progress" 확인 → 점유 중이면 스킵
3. **타입이 자동화에 적합한가?**
   - bug, design-debt, test, docs, infra → 후보
   - security, perf → 코드 분석 후 판단
4. **실제 코드를 확인했을 때 수정 방향이 명확한가?**
   - 가이드(`docs/apex_core/apex_core_guide.md`, `CLAUDE.md`)에 답이 있는가?
   - 기존 코드 패턴을 따르면 되는가?
   - **YES → 번들에 포함**
   - **NO → 드롭** (백로그 설명란에 `[FSD 분석]` 메모 추가)

### 번들 구성

- 선별된 항목들을 스코프 연관성 + 파일 겹침 기준으로 자연스럽게 묶는다
- 번들 크기는 자율 판단
- 현재 섹션(NOW/IN VIEW/DEFERRED)에서 번들이 0개면 다음 섹션으로 넘어간다
- **모든 섹션을 확인해도 번들이 0개면 브랜치 생성 없이 분석 결과만 리포트하고 종료**

### 드롭 메모 형식

백로그 항목의 기존 설명 필드 끝에 추가:

```
  **[FSD 분석 YYYY-MM-DD]** 분석 사유를 구체적으로 기술.
```

---

## Phase 2: 착수

**번들이 확정된 후에만 이 Phase에 진입한다.**

아래 핸드오프 워크플로우를 정확히 따른다:

```bash
# 1. 점유 현황 확인 (Phase 1에서 이미 확인했지만, 착수 직전 재확인)
apex-agent handoff status

# 2. 브랜치 생성 (최신 main 기반, enforce-rebase hook이 자동 보장)
#    타임스탬프는 date +"%Y%m%d_%H%M%S" 명령으로 정확한 현재 시각을 취득
git checkout -b feature/fsd-backlog-<YYYYMMDD_HHMMSS>

# 3. 착수 선언 (--summary 필수, 비우면 hook이 차단)
#    --backlog는 숫자만 전달 (접두사는 자동 추가)
apex-agent handoff notify start \
  --skip-design \
  --backlog "<N,M,...>" \
  --scopes "<스코프1,스코프2>" \
  --summary "BACKLOG-N,M,... 백로그 소탕"
```

> **주의**: `init`이 아니라 `notify start`다. `init`은 저장소 초기화 명령이며, 착수 선언은 `notify start`로 한다.

---

## Phase 3: 구현

기존 CLAUDE.md 워크플로우를 그대로 따른다 (clang-format → 빌드 → 테스트).

### 구현 중 드롭

파보니까 예상보다 복잡하여 설계 결정이 필요한 항목이 발견되면:
1. 해당 항목의 변경만 revert (항목별 커밋 분리 또는 수동 revert)
2. 백로그 설명란에 `[FSD 분석]` 메모 추가
3. 나머지 항목은 계속 진행

### 커밋 메시지

번들 내 항목 성격에 맞는 conventional commit 타입을 선택한다:

- bug → `fix`, test → `test`, docs → `docs`, infra → `chore`, design-debt → `refactor` 등
- 형태: `<타입>(<스코프>): BACKLOG-N,M,... 백로그 소탕`

---

## Phase 4: 이후 기존 워크플로우

리뷰, 문서 갱신, PR, CI, 머지는 전부 기존 CLAUDE.md 지침 + 자율 판단으로 진행한다.

### 머지 완료 후

```bash
# 점유 릴리즈 (active + backlog-status 자동 정리)
apex-agent handoff notify merge \
  --summary "BACKLOG-N,M,... 소탕 완료"
```

---

## 결과 리포트

모든 작업이 완료되면 터미널에 소탕 결과를 출력한다:

```
═══════════════════════════════════════
  FSD Backlog Sweep — 완료
═══════════════════════════════════════

  브랜치: feature/fsd-backlog-<타임스탬프>
  PR: #N (squash merged)

  해결: BACKLOG-N, BACKLOG-M, ...
  드롭: BACKLOG-X (사유 한 줄, 설명란 메모 완료)

═══════════════════════════════════════
```

번들이 전부 드롭되어 구현할 게 없었다면:

```
═══════════════════════════════════════
  FSD Backlog Sweep — 소탕 대상 없음
═══════════════════════════════════════

  스캔 완료: NOW (N건) → IN VIEW (N건) → DEFERRED (N건)
  자동화 가능 항목: 0건

  드롭: BACKLOG-X (사유)
        BACKLOG-Y (사유)

═══════════════════════════════════════
```
