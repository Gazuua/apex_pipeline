# BACKLOG-110, 111 구현 계획 — 단계 기반 워크플로우 + CLAUDE.md 컴팩션

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 7단계 워크플로우를 루트 CLAUDE.md에 정의하고, 6개 CLAUDE.md 파일을 컴팩션하여 ~20-25% 감소

**Architecture:** 문서 전용 변경. 루트 CLAUDE.md에 워크플로우 섹션 신설 → 이를 기준으로 6개 파일의 중복·원본 참조 가능 정보·장황한 규칙 정비

**Tech Stack:** Markdown, 기존 hook 인프라 (변경 없음)

**Spec:** `docs/apex_common/plans/20260321_113435_workflow-and-docs-compaction-design.md`

**핵심 규칙:** 모든 파일 변경은 사용자와 합의 후 적용. 일괄 변경 금지.

---

## Phase 1: 워크플로우 단계 추가 (#110)

### Task 1: 루트 CLAUDE.md에 워크플로우 섹션 신설

**Files:**
- Modify: `CLAUDE.md:1-10` (개요 직후에 섹션 삽입)

**내용:** 7단계 풀 + 스킵 조건 + 체크리스트 설명을 `## 워크플로우` 섹션으로 추가. 위치: `## 개요` 직후, `## 모노레포 구조` 직전.

- [ ] **Step 1: 워크플로우 섹션 초안 작성**

`## 개요` 블록 직후에 아래 내용 삽입:

```markdown
## 워크플로우

모든 작업은 아래 7단계 중 해당하는 단계를 순서대로 밟는다. 착수 시 스킵 조건을 평가하여 체크리스트(TaskCreate)를 생성한다.

| # | 단계 | 핵심 행위 | 산출물 |
|---|------|-----------|--------|
| ① | **착수** | 브랜치 생성, 백로그 확인, 핸드오프 backlog-check | 브랜치 |
| ② | **설계** | 브레인스토밍 → 스펙 문서 | `docs/{project}/plans/` |
| ③ | **구현** | 코드 작성, clang-format | 소스 변경 |
| ④ | **검증** | 빌드 + 테스트 (Debug, 필요시 Release/ASAN/TSAN. queue-lock.sh 경유) | 빌드 성공 |
| ⑤ | **리뷰** | auto-review 실행, 이슈 수정 | `docs/{project}/review/` |
| ⑥ | **문서 갱신** | CLAUDE.md, Apex_Pipeline.md, BACKLOG.md, README, progress 등 | 갱신된 문서 |
| ⑦ | **머지** | lock 획득 → rebase → (빌드) → push → squash merge → lock 해제 → 브랜치 정리 | main에 머지 |

**스킵 조건:**

| 단계 | 스킵 가능 조건 |
|------|---------------|
| ② 설계 | 변경이 단순하고 설계 판단이 불필요 (오타 수정, 1파일 이하 기계적 변경, 문서 전용) |
| ③ 구현 | 문서 전용 작업 |
| ④ 검증 | 문서 전용 작업 (코드 변경 없음) |
| ⑤ 리뷰 | 문서 전용 작업, 또는 변경 범위가 극히 작아 리뷰 ROI가 없는 경우 |
| ①⑥⑦ | **스킵 불가** — 모든 작업에 필수 |

기존 hook 4개가 도구 호출 시 핵심 게이트를 강제한다 (빌드 경로, 머지 lock, 핸드오프). 상세: `settings.json` + `.claude/hooks/`.
```

- [ ] **Step 2: 사용자 확인**

추가된 워크플로우 섹션을 사용자에게 보여주고 승인 받기.

- [ ] **Step 3: 적용**

승인 후 Edit으로 루트 CLAUDE.md에 삽입.

---

### Task 2: Git 섹션 머지 절차 중복 제거

**Files:**
- Modify: `CLAUDE.md:46-56` (Git / 브랜치 섹션의 머지 절차)

**내용:** 스펙에 따라 워크플로우 ⑦은 요약만 기술하고, 머지 상세 절차(빌드 스킵 조건 포함)는 Git 섹션에 유지한다. 워크플로우 ⑦에서 Git 섹션을 포인터로 참조. 단, 머지 상세의 표현 간결화 여부는 사용자와 논의.

- [ ] **Step 1: 워크플로우 ⑦에 포인터 확인 및 Git 섹션 간결화 제안**

워크플로우 ⑦ 행의 "핵심 행위"에 "상세: § Git/브랜치 머지 참조" 포인터가 있는지 확인. Git 섹션 머지 절차의 표현 간결화 가능 여부를 사용자에게 제시.

- [ ] **Step 2: 사용자 결정에 따라 적용**

- [ ] **Step 3: 커밋**

```bash
git add CLAUDE.md
git commit -m "feat(docs): BACKLOG-110 워크플로우 7단계 정의 — 루트 CLAUDE.md에 추가"
git push
```

---

## Phase 2: CLAUDE.md 컴팩션 (#111)

컴팩션 원칙:
1. 원본이 있는 정보는 삭제
2. 중복은 한 곳으로
3. 단순 규칙은 1줄로

파일별로 "제안 → 합의 → 적용" 순서. 감소 폭이 큰 파일부터 진행.

---

### Task 3: e2e/CLAUDE.md 컴팩션 (예상 -50~70줄)

**Files:**
- Modify: `apex_services/tests/e2e/CLAUDE.md` (현재 165줄)

**삭제 후보:**

| 영역 | 줄수 (추정) | 삭제 사유 |
|------|------------|-----------|
| 서비스 프로세스 관계 compose 구조도 | ~15줄 | `docker-compose.e2e.yml` 원본 참조 |
| 환경변수 표 | ~10줄 | 코드/toml에 정의됨 |
| E2E/스트레스 테스트 구조 표 | ~20줄 | 소스 파일에서 유추 가능 |
| CI 실행 방식 yml 코드 블록 | ~15줄 | `.github/workflows/` 원본 참조 |
| 트러블슈팅 상세 (6개 항목) | ~50줄 중 일부 | 항목 자체는 유지하되 장황한 설명 간결화 |

- [ ] **Step 1: 구체적 삭제/간결화 제안을 사용자에게 제시**

파일을 읽고 "이 블록 삭제 / 이 블록 N줄로 간결화" 형태로 제안.

- [ ] **Step 2: 사용자 확인 후 적용**

- [ ] **Step 3: 줄수 확인** — `wc -l` 으로 감소 확인

---

### Task 4: docs/CLAUDE.md 컴팩션 (예상 -15~20줄)

**Files:**
- Modify: `docs/CLAUDE.md` (현재 112줄)

**범위:** 백로그 운영 규칙 (~70줄)은 컴팩션 대상이 아님 (권위 소스).

**삭제 후보:**

| 영역 | 삭제 사유 |
|------|-----------|
| 코드 리뷰 섹션의 clangd LSP 전략 상세 | 현행 유지 또는 간결화 — 사용자와 논의 |
| 브레인스토밍 섹션 `Apex_Pipeline.md` 읽기 규칙 | 워크플로우 ② 설계에 포함 가능 여부 검토 |
| 설계 문서 경로 규칙 | 루트 CLAUDE.md 문서 규칙과 중복 |
| 파일명/타임스탬프 규칙 | 루트 CLAUDE.md에 이미 있음 |

- [ ] **Step 1: 구체적 삭제/간결화 제안을 사용자에게 제시**
- [ ] **Step 2: 사용자 확인 후 적용**
- [ ] **Step 3: 줄수 확인**

---

### Task 5: apex_core/CLAUDE.md 컴팩션 (예상 -10~15줄)

**Files:**
- Modify: `apex_core/CLAUDE.md` (현재 53줄)

**삭제 후보:**

| 영역 | 삭제 사유 |
|------|-----------|
| vcpkg 의존성 목록 (line 15) | `vcpkg.json` 원본 참조 |
| 빌드 명령 예시 (line 8-9) | 루트 CLAUDE.md `## 빌드`와 중복 |
| `build.bat` 직접 호출 관련 | 루트에서 이미 기술 + hook이 강제 |
| 빌드 변형 설명 | 루트에서 포인터로 참조하므로 여기서는 유지 — 사용자 판단 |

- [ ] **Step 1: 구체적 삭제/간결화 제안을 사용자에게 제시**
- [ ] **Step 2: 사용자 확인 후 적용**
- [ ] **Step 3: 줄수 확인**

---

### Task 6: apex_tools/CLAUDE.md 컴팩션 (예상 -5줄)

**Files:**
- Modify: `apex_tools/CLAUDE.md` (현재 34줄)

**삭제 후보:**

| 영역 | 삭제 사유 |
|------|-----------|
| 빌드 관련 섹션의 `run_in_background` 규칙 | 루트 CLAUDE.md `## 빌드`와 중복 |
| 빌드 동시 실행 금지 | 루트에서 이미 기술 |

- [ ] **Step 1: 구체적 삭제/간결화 제안을 사용자에게 제시**
- [ ] **Step 2: 사용자 확인 후 적용**
- [ ] **Step 3: 줄수 확인**

---

### Task 7: .github/CLAUDE.md 확인

**Files:**
- Review: `.github/CLAUDE.md` (현재 15줄)

- [ ] **Step 1: 파일 검토** — 변경 필요 여부 판단. 이미 간결하므로 변경 없이 통과 가능성 높음.

---

### Task 8: 루트 CLAUDE.md 컴팩션 (예상 -30줄, 워크플로우 추가 후 순감 ~10줄)

**Files:**
- Modify: `CLAUDE.md` (Task 1-2 적용 후 상태 기준)

**의존성:** 머지 절차 관련 컴팩션은 Task 2의 결정 결과에 따라 범위가 달라짐. Task 2에서 Git 섹션 머지 상세를 유지한 경우, 여기서는 머지 관련 추가 삭제 없음.

**삭제 후보:** Task 1에서 워크플로우를 추가한 뒤, 기존 섹션과 겹치는 부분 정리.

| 영역 | 삭제 사유 |
|------|-----------|
| 빌드 섹션: sub-CLAUDE.md에서 삭제된 규칙의 "상세 포인터" 정리 | 더 이상 가리킬 중복이 없으면 포인터 불필요 |
| 저작권 헤더, 포맷팅: 간결화 가능 여부 | 이미 1-2줄이므로 대상 아닐 수 있음 |
| 에이전트 작업 섹션 | 워크플로우 체크리스트와 겹치는 부분 확인 |

- [ ] **Step 1: 현재 상태에서 추가 컴팩션 제안**
- [ ] **Step 2: 사용자 확인 후 적용**
- [ ] **Step 3: 줄수 확인**

---

### Task 9: 파일별 커밋

각 파일 컴팩션 완료 시 즉시 커밋+푸시. 세션 중단 시 작업 손실 방지. "커밋 즉시 리모트 푸시" 규칙 준수.

- [ ] **Step 1: Task 3~8 각 완료 시 해당 파일 커밋**

```bash
# 예시 (각 파일별 반복):
git add apex_services/tests/e2e/CLAUDE.md
git commit -m "docs(claude): e2e/CLAUDE.md 컴팩션 — 원본 참조 전환·간결화"
git push
```

**참고:** 파일별 커밋이 과도하면 2~3개씩 묶어 커밋해도 됨. 단, Phase 2 전체를 하나로 묶지는 않는다.

---

## Phase 3: 마무리

### Task 10: 최종 검증

- [ ] **Step 1: 전체 줄수 비교**

```bash
wc -l CLAUDE.md docs/CLAUDE.md apex_core/CLAUDE.md apex_tools/CLAUDE.md \
      .github/CLAUDE.md apex_services/tests/e2e/CLAUDE.md
```

목표: 502줄 → ~390줄 (20-25% 감소). 실제 감소율 확인.

- [ ] **Step 2: 교차 참조 검증**

각 CLAUDE.md에서 다른 파일을 포인터로 참조하는 부분이 실제로 존재하는지 확인. 삭제된 섹션을 가리키는 깨진 포인터가 없어야 함.

---

### Task 11: 리뷰 판단 (⑤단계)

- [ ] **Step 1: auto-review 필요 여부 판단**

문서 전용 PR이므로 스킵 조건 "문서 전용 작업"에 해당. auto-review 스킵. 단, 워크플로우 정의가 기존 규칙과 상충하는 부분이 발견되면 재검토.

---

### Task 12: 문서 갱신 (⑥단계)

- [ ] **Step 1: BACKLOG.md에서 #110, #111 삭제 → BACKLOG_HISTORY.md에 이전**
- [ ] **Step 2: progress 문서 작성** — `docs/apex_common/progress/` 에 결과 요약
- [ ] **Step 3: Apex_Pipeline.md 갱신 필요 여부 확인** — 이번 변경은 아키텍처 영향 없으므로 불필요할 가능성 높음
- [ ] **Step 4: README.md 갱신 필요 여부 확인**
- [ ] **Step 5: 커밋**

```bash
git add docs/BACKLOG.md docs/BACKLOG_HISTORY.md docs/apex_common/progress/
git commit -m "docs: BACKLOG-110, 111 완료 문서 갱신"
git push
```

---

### Task 13: 머지 (⑦단계)

- [ ] **Step 1: PR 생성**
- [ ] **Step 2: CI 통과 확인** (문서 전용이므로 빌드 스킵 예상)
- [ ] **Step 3: 머지 절차 실행**

```bash
PROJECT_ROOT="$(cd "$(git rev-parse --show-toplevel)" && pwd)"
"${PROJECT_ROOT}/apex_tools/queue-lock.sh" merge acquire
git fetch origin main && git rebase origin/main
# 빌드 스킵: 문서 전용 PR — 머지 절차 스킵 조건 A(①+②) 충족
git push --force-with-lease
gh pr merge --squash --admin
"${PROJECT_ROOT}/apex_tools/queue-lock.sh" merge release
```

- [ ] **Step 4: 브랜치 정리**

```bash
"${PROJECT_ROOT}/apex_tools/cleanup-branches.sh" --execute
```
