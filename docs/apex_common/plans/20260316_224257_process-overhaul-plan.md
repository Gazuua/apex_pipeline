# 프로세스 오버홀 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 에이전트 드리븐 작업 방식, auto-review 프로세스, 팀 구조를 전면 재설계하여 품질·정확성·연속성을 확보한다.

**Architecture:** SendMessage 팀 구조를 Agent tool 서브에이전트로 전환. 3-layer (Main→Coordinator→12 Reviewers)를 2-layer (Main→최대 7 Reviewers)로 단순화. 메인이 코드를 직접 읽고, 디스패치하고, 결과를 검증하며, cross-cutting을 직접 수행한다.

**설계서:** `docs/apex_common/plans/20260316_222855_process-overhaul-design.md`

**핵심 원칙:** 목적만 전달하고 방법은 에이전트 자율에 맡긴다. 과도한 제약은 제거한다.

---

## Chunk 1: CLAUDE.md 정리

### Task 1: 프로젝트 CLAUDE.md 에이전트 규칙 재작성

**Files:**
- Modify: `D:\.workspace\CLAUDE.md` (lines 21, 64-69)

- [ ] **Step 1: 에이전트 작업 섹션 재작성**

Lines 64-69의 기존 규칙을 아래로 교체:

```markdown
### 에이전트 작업
- **서브에이전트는 작업 전 관련 CLAUDE.md를 직접 읽는다** — 메인이 발췌할 필요 없이 서브에이전트가 해당 영역의 CLAUDE.md를 직접 Read하여 규칙 파악
- **태스크 완료 후 메인이 auto-review 필요 여부를 자체 판단하여 실행한다** (유저에게 묻지 않음)
- **리뷰 이슈 판단 기준**: "지금 안 고치면 나중에 더 복잡해지는가?" YES면 지금 수정. 스코프 밖이라는 이유로 미루지 않음. 리뷰어는 구현 비용보다 미래 비용을 우선 고려
```

- [ ] **Step 2: 병렬 빌드 조율 규칙 수정**

Line 21의 기존:
```
- **병렬 에이전트 빌드 조율**: 여러 에이전트 동시 작업 시 빌드는 메인 에이전트(또는 coordinator)만 수행. 워커 에이전트는 코드 수정만 하고 빌드를 직접 실행하지 않음
```

아래로 교체:
```
- **병렬 에이전트 빌드 조율**: 여러 에이전트 동시 작업 시 빌드는 한 번에 하나만 수행 (동시 빌드 시 시스템 렉)
```

- [ ] **Step 3: 커밋**

```bash
git add CLAUDE.md
git commit -m "docs(rules): 에이전트 작업 규칙을 자율 판단 기반으로 재작성"
```

---

### Task 2: apex_tools/CLAUDE.md auto-review 규칙 수정

**Files:**
- Modify: `D:\.workspace\apex_tools\CLAUDE.md` (lines 23-24)

- [ ] **Step 1: auto-review 섹션 재작성**

Lines 23-24의 기존:
```
- 태스크 완료 후 `/auto-review task` 묻지 말고 자동 실행 — 리뷰 → 수정 → 재리뷰 → Clean → PR+CI 전 과정 자동화
- coordinator/리뷰어가 정의된 프로세스대로 동작 중이면 재촉하지 않고 기다림. 프로세스에 report 전송이 명시되어 있으면 요청 없이 자동 전송될 때까지 대기
```

아래로 교체:
```
- 메인이 auto-review 필요 여부를 자체 판단하여 실행한다 (유저에게 묻지 않음)
```

- [ ] **Step 2: 커밋**

```bash
git add apex_tools/CLAUDE.md
git commit -m "docs(rules): apex_tools auto-review 규칙 간소화"
```

---

## Chunk 2: 에이전트 파일 재편

### Task 3: Coordinator 및 폐기 리뷰어 삭제

**Files:**
- Delete: `apex_tools/claude-plugin/agents/coordinator.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-memory.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-concurrency.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-api.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-architecture.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-test-coverage.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-test-quality.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-security.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-infra.md`
- Delete: `apex_tools/claude-plugin/agents/reviewer-cross-cutting.md`

- [ ] **Step 1: 파일 삭제**

```bash
cd apex_tools/claude-plugin/agents/
git rm coordinator.md
git rm reviewer-memory.md reviewer-concurrency.md
git rm reviewer-api.md reviewer-architecture.md
git rm reviewer-test-coverage.md reviewer-test-quality.md
git rm reviewer-security.md reviewer-infra.md
git rm reviewer-cross-cutting.md
```

- [ ] **Step 2: 커밋**

```bash
git commit -m "refactor(agents): coordinator + 10 리뷰어 삭제 (7명 체제 전환 준비)"
```

---

### Task 4: 유지 리뷰어 3개 업데이트

기존 reviewer-docs-spec.md, reviewer-docs-records.md, reviewer-logic.md를 유지하되, 과도한 행동 제약을 제거하고 자율 판단 기반으로 재작성한다.

**Files:**
- Modify: `apex_tools/claude-plugin/agents/reviewer-docs-spec.md`
- Modify: `apex_tools/claude-plugin/agents/reviewer-docs-records.md`
- Modify: `apex_tools/claude-plugin/agents/reviewer-logic.md`

- [ ] **Step 1: 각 파일의 기존 내용 읽기**

기존 내용을 먼저 읽어서 도메인 범위와 프로젝트 고유 맥락(검토 대상 파일 패턴, 도메인 지식 등)을 파악한다.

- [ ] **Step 2: 과도한 제약 식별 및 제거**

각 파일에서 아래 패턴에 해당하는 내용을 제거 또는 완화:
- 보고 포맷 강제 (예: `[수정됨]`, `[에스컬레이션]` 태그 필수)
- severity 분류 기준 강제 (예: confidence ≥50만 보고)
- 리뷰 순서/절차 강제
- SendMessage/coordinator 관련 프로토콜
- 수정 전략 강제 (예: "signature 변경 시 call-site 검색 필수")

유지할 내용:
- 프론트매터 (name, description, model, color)
- 도메인 범위 설명 (무엇을 검토하는지)
- 프로젝트 고유 맥락 (이 프로젝트에서 특히 주의할 점)

핵심: **"이 도메인에서 무엇을 검토하고 왜 중요한지"만 남기고, "어떻게 검토하라"는 제거.**

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-docs-spec.md
git add apex_tools/claude-plugin/agents/reviewer-docs-records.md
git add apex_tools/claude-plugin/agents/reviewer-logic.md
git commit -m "refactor(agents): 유지 리뷰어 3개 자율 판단 기반으로 재작성"
```

---

### Task 5: 신규 리뷰어 4개 생성

기존 도메인들을 통합한 새 리뷰어 에이전트를 생성한다.

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-systems.md` (memory + concurrency 통합)
- Create: `apex_tools/claude-plugin/agents/reviewer-design.md` (api + architecture 통합)
- Create: `apex_tools/claude-plugin/agents/reviewer-test.md` (test-coverage + test-quality 통합)
- Create: `apex_tools/claude-plugin/agents/reviewer-infra-security.md` (infra + security 통합)

- [ ] **Step 1: 삭제된 리뷰어들의 git history에서 도메인 지식 확인**

통합 대상 기존 파일들의 마지막 커밋 내용을 참고하여 도메인 범위와 프로젝트 고유 맥락을 파악한다:

```bash
git show HEAD~1:apex_tools/claude-plugin/agents/reviewer-memory.md | head -30
git show HEAD~1:apex_tools/claude-plugin/agents/reviewer-concurrency.md | head -30
# ... 나머지도 동일 (HEAD~1 = Task 3 삭제 커밋 이전)
```

- [ ] **Step 2: 4개 신규 파일 작성**

각 파일의 구조:

```markdown
---
name: reviewer-{domain}
description: {한국어 도메인 설명}
model: opus
color: {적절한 색상}
---

# {도메인} 리뷰어

## 목적
{이 리뷰어가 왜 존재하는지, 무엇을 지키려는 것인지}

## 도메인 범위
{통합된 영역들의 검토 대상 나열}

## 프로젝트 맥락
{이 프로젝트에서 특히 중요한 도메인 고유 사항}
```

**핵심:** 검토 방법, 보고 형식, severity 기준 등 방법론은 적지 않는다. 목적과 범위만.

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-systems.md
git add apex_tools/claude-plugin/agents/reviewer-design.md
git add apex_tools/claude-plugin/agents/reviewer-test.md
git add apex_tools/claude-plugin/agents/reviewer-infra-security.md
git commit -m "feat(agents): 통합 리뷰어 4개 생성 (systems, design, test, infra-security)"
```

---

## Chunk 3: Auto-review 명령 재작성

### Task 6: auto-review.md 전면 재작성

기존 19KB 5-Phase 구조를 자율 판단 기반 간결한 명령으로 재작성한다.

**Files:**
- Rewrite: `apex_tools/claude-plugin/commands/auto-review.md`

- [ ] **Step 1: 기존 내용 읽기**

전체 파일을 읽어서 유지해야 할 내용 (안전 제약, 프로젝트 고유 규칙)과 제거할 내용 (팀 구조, Phase 강제, 포맷 강제)을 식별한다.

- [ ] **Step 2: 재작성**

새 구조:

```markdown
---
description: "Auto-review: 서브에이전트 기반 코드 리뷰 + 수정 자동화"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent"]
---

# Auto-review

## 목적
변경 코드의 품질을 높인다. 미발견 버그, 부실한 구현, 정합성 문제, 더 나은 개선 방안을 포괄적으로 검토하여 수정한다.

## 리뷰어 (최대 7명)
{7명 도메인 테이블 — 설계서 §4 참조}

## 흐름
1. 변경 분석: diff 파악, 변경 파일 읽기
2. 리뷰어 디스패치: 필요한 리뷰어를 판단하여 Agent tool로 스폰
3. 결과 종합: cross-cutting 분석, 필요 시 추가 조치
4. 마무리: 빌드, 리뷰 문서, PR, CI

{각 단계의 목적만 기술, 구체적 방법은 기술하지 않음}

## 안전 제약
- 무한 루프 방지 (라운드/이슈/CI 반복에 대한 상한 개념 유지)
- 빌드는 한 번에 하나만
- main 직접 커밋 금지

## 모드
- `task`: 현재 브랜치의 변경사항 리뷰
- `full`: 전체 코드베이스 리뷰
```

**핵심:** 기존 Phase 0-5 강제 흐름, SendMessage 프로토콜, 팀 생성/해산, 보고 포맷 강제, 좀비 처리 로직 등 전부 제거. 메인의 자율 판단에 맡긴다.

- [ ] **Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/commands/auto-review.md
git commit -m "feat(auto-review): Agent tool 기반 자율 판단 구조로 전면 재작성"
```

---

## Chunk 4: Config, Plugin, 백로그 마무리

### Task 7: config.md 업데이트

**Files:**
- Modify: `apex_tools/auto-review/config.md`

- [ ] **Step 1: 기존 내용 읽기 및 재작성**

변경 내용:
- 리뷰어 목록: 12명 → 7명 (새 이름 반영: systems, design, test, infra-security)
- 스마트 스킵 매핑: 새 도메인 이름으로 업데이트
- coordinator 관련 내용 제거
- Round 1/Round 2 구분 제거 (cross-cutting은 메인이 직접)
- 전역 설정 (round_limit 등): 참고값으로 유지하되 "강제"가 아닌 "메인 판단 참고"로 표현 변경

- [ ] **Step 2: 커밋**

```bash
git add apex_tools/auto-review/config.md
git commit -m "refactor(config): 7명 리뷰어 체제 + 메인 판단 참고용으로 전환"
```

---

### Task 8: plugin.json 업데이트

**Files:**
- Modify: `apex_tools/claude-plugin/.claude-plugin/plugin.json`

- [ ] **Step 1: description과 version 업데이트**

```json
{
  "name": "apex-auto-review",
  "description": "Agent tool 기반 서브에이전트 리뷰 + 수정 자동화 for Apex Pipeline",
  "version": "3.0.0",
  "author": {"name": "Gazuua"}
}
```

- [ ] **Step 2: 커밋**

```bash
git add apex_tools/claude-plugin/.claude-plugin/plugin.json
git commit -m "chore(plugin): v3.0.0 — Agent tool 기반 구조 전환 반영"
```

- [ ] **Step 3: 플러그인 캐시 삭제**

버전 변경 후 캐시 삭제 필수 (apex_tools/CLAUDE.md 규칙):

```bash
rm -rf ~/.claude/plugins/cache/apex-local/apex-auto-review/
```

---

### Task 9: 백로그 좀비 이슈 완료 처리

**Files:**
- Modify: `docs/BACKLOG.md` (lines 258-267)

- [ ] **Step 1: 좀비 에이전트 이슈 삭제**

Lines 258-267의 "[Medium] auto-review 팀 셧다운 시 좀비 에이전트 잔존 문제" 항목을 삭제한다. (CLAUDE.md 규칙: 완료 항목은 즉시 삭제, git이 이력 보존)

- [ ] **Step 2: 커밋**

```bash
git add docs/BACKLOG.md
git commit -m "docs(backlog): 좀비 에이전트 이슈 완료 처리 (Agent tool 전환으로 구조적 해결)"
```

---

## Chunk 5: 검증

### Task 10: 전체 정합성 검증

- [ ] **Step 1: 폐기된 용어 잔존 확인**

프로젝트 전체에서 아래 용어를 검색하여 잔존 여부 확인:

```bash
grep -r "coordinator" . --include="*.md" -l
grep -r "SendMessage" . --include="*.md" -l
grep -r "TeamCreate\|TeamDelete" . --include="*.md" -l
grep -r "reviewer-memory\|reviewer-concurrency\|reviewer-api\|reviewer-architecture" . --include="*.md" -l
grep -r "reviewer-test-coverage\|reviewer-test-quality\|reviewer-security\b\|reviewer-infra\b" . --include="*.md" -l
grep -r "reviewer-cross-cutting" . --include="*.md" -l
grep -r "메인 컨텍스트 절약" . --include="*.md" -l
grep -r "에이전트 팀 병렬" . --include="*.md" -l
```

잔존 발견 시 해당 파일에서 수정 또는 제거.

- [ ] **Step 2: 에이전트 파일 목록 확인**

```bash
ls apex_tools/claude-plugin/agents/
```

기대 결과 (7개):
```
reviewer-docs-spec.md
reviewer-docs-records.md
reviewer-logic.md
reviewer-systems.md
reviewer-design.md
reviewer-test.md
reviewer-infra-security.md
```

- [ ] **Step 3: CLAUDE.md 규칙 일관성 확인**

프로젝트 CLAUDE.md와 apex_tools/CLAUDE.md의 에이전트 관련 규칙이 상충하지 않는지 확인.

- [ ] **Step 4: 최종 커밋 (잔존 수정이 있는 경우)**

```bash
git add -A
git commit -m "fix: 프로세스 오버홀 정합성 검증 후 잔존 항목 수정"
```
