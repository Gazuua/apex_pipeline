# 문서 구조 통합 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 분산된 프로젝트 문서를 docs/로 통합하고 프로젝트별 하위 구조로 재분류 (apex_docs → docs 리네임 포함)

**Architecture:** 파일 이동 + 디렉토리 재구성. 코드 변경 없음. CLAUDE.md/메모리 경로 동기화 포함.

**Tech Stack:** git mv, bash

**설계 문서:** `docs/plans/20260308_105306_docs-consolidation-design.md`

---

### Task 1: 브랜치 + 워크트리 생성

**Step 1: feature 브랜치와 워크트리 생성**

```bash
cd D:/.workspace
git worktree add .worktrees/feature_docs_consolidation -b feature/docs-consolidation
```

**Step 2: 워크트리에서 작업 확인**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git branch --show-current
```
Expected: `feature/docs-consolidation`

---

### Task 2: apex_docs/ → docs/ 리네임

**Step 1: 디렉토리 리네임**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git mv apex_docs docs
```

**Step 2: 프로젝트별 하위 디렉토리 생성**

```bash
mkdir -p docs/apex_core/{plans,progress,review}
mkdir -p docs/apex_infra/{plans,progress,review}
mkdir -p docs/apex_shared/{plans,progress,review}
```

**Step 3: 커밋**

```bash
git add -A
git commit -m "docs: apex_docs/ → docs/ 리네임 + 프로젝트별 하위 구조 생성"
```

---

### Task 3: apex_core/docs/ → docs/apex_core/ 이동 (37건)

**Files:**
- Move: `apex_core/docs/design-decisions.md` → `docs/apex_core/design-decisions.md`
- Move: `apex_core/docs/design-rationale.md` → `docs/apex_core/design-rationale.md`
- Move: `apex_core/docs/plans/*` (16건) → `docs/apex_core/plans/`
- Move: `apex_core/docs/progress/*` (11건) → `docs/apex_core/progress/`
- Move: `apex_core/docs/review/*` (9건) → `docs/apex_core/review/`

**Step 1: 설계 문서 이동**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git mv apex_core/docs/design-decisions.md docs/apex_core/
git mv apex_core/docs/design-rationale.md docs/apex_core/
```

**Step 2: plans 이동 (16건)**

```bash
git mv apex_core/docs/plans/* docs/apex_core/plans/
```

**Step 3: progress 이동 (11건)**

```bash
git mv apex_core/docs/progress/* docs/apex_core/progress/
```

**Step 4: review 이동 (9건)**

```bash
git mv apex_core/docs/review/* docs/apex_core/review/
```

**Step 5: 검증 — 원본 디렉토리 비어있는지 확인**

```bash
find apex_core/docs/ -type f
```
Expected: 출력 없음 (빈 디렉토리)

**Step 6: apex_core/docs/ 삭제**

```bash
rm -rf apex_core/docs/
```

**Step 7: 커밋**

```bash
git add -A
git commit -m "docs: apex_core/docs/ → docs/apex_core/ 이동 (37건)"
```

---

### Task 4: docs/ 내부 재분류 — 인프라 문서

**Files:**
- Move: `docs/plans/20260307_204613_docker-compose-design.md` → `docs/apex_infra/plans/`
- Move: `docs/plans/20260307_204652_docker-compose-implementation.md` → `docs/apex_infra/plans/`
- Copy: `docs/plans/20260307_202237_monorepo-infra-and-shared.md` → `docs/apex_infra/plans/` (걸치는 문서 — 복사)

**Step 1: 인프라 전용 문서 이동**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git mv docs/plans/20260307_204613_docker-compose-design.md docs/apex_infra/plans/
git mv docs/plans/20260307_204652_docker-compose-implementation.md docs/apex_infra/plans/
```

**Step 2: 걸치는 문서 복사 (원본은 Task 5 Step 2에서 이동)**

```bash
cp docs/plans/20260307_202237_monorepo-infra-and-shared.md docs/apex_infra/plans/
```

---

### Task 5: docs/ 내부 재분류 — shared 문서

**Files:**
- Move: `docs/plans/20260307_234047_apex-shared-build-infra-design.md` → `docs/apex_shared/plans/`
- Move: `docs/plans/20260307_234129_apex-shared-build-infra-implementation.md` → `docs/apex_shared/plans/`
- Move: `docs/plans/20260307_202237_monorepo-infra-and-shared.md` → `docs/apex_shared/plans/` (걸치는 문서 — 원본 이동)
- Move: `docs/progress/20260308_000627_apex-shared-build-infra.md` → `docs/apex_shared/progress/`

**Step 1: shared 전용 문서 이동**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git mv docs/plans/20260307_234047_apex-shared-build-infra-design.md docs/apex_shared/plans/
git mv docs/plans/20260307_234129_apex-shared-build-infra-implementation.md docs/apex_shared/plans/
```

**Step 2: 걸치는 문서 원본 이동**

```bash
git mv docs/plans/20260307_202237_monorepo-infra-and-shared.md docs/apex_shared/plans/
```

**Step 3: progress 이동**

```bash
git mv docs/progress/20260308_000627_apex-shared-build-infra.md docs/apex_shared/progress/
```

---

### Task 6: docs/ 내부 재분류 — 코어 리뷰

**Files:**
- Move: `docs/review/20260308_101819_v0.2.4_comprehensive-review.md` → `docs/apex_core/review/`

**Step 1: v0.2.4 리뷰 보고서 이동**

```bash
cd D:/.workspace/.worktrees/feature_docs_consolidation
git mv docs/review/20260308_101819_v0.2.4_comprehensive-review.md docs/apex_core/review/
```

**Step 2: 재분류 일괄 커밋**

```bash
git add -A
git commit -m "docs: docs/ 내부 재분류 — 인프라/shared/core 프로젝트별 분류"
```

**Step 3: 검증 — 최종 구조 확인**

```bash
find docs/ -type f -name "*.md" | sort
```
Expected: 모든 파일이 설계 문서의 분류에 맞게 배치됨

---

### Task 7: apex_core/README.md 생성

**Files:**
- Create: `apex_core/README.md`

**Step 1: README 작성**

apex_core의 목적, 빌드 방법, 주요 컴포넌트를 간결하게 기술.
기존 CLAUDE.md의 apex_core 섹션과 Apex_Pipeline.md를 참고하여 작성.

**Step 2: 커밋**

```bash
git add apex_core/README.md
git commit -m "docs: apex_core README.md 추가"
```

---

### Task 8: CLAUDE.md + 메모리 경로 업데이트

**Files:**
- Modify: `CLAUDE.md` — 모노레포 구조 섹션, 설계 문서 경로, 모든 `apex_docs` → `docs` 변경
- Modify: `C:\Users\JHG\.claude\projects\D---workspace\memory\MEMORY.md` — 구조 섹션

**Step 1: CLAUDE.md 수정**

- 모노레포 구조에서 `apex_core/docs/` 항목 제거
- `apex_docs/` → `docs/` 전체 반영 (하위 구조 포함)
- 설계 문서 경로: `apex_core/docs/design-decisions.md` → `docs/apex_core/design-decisions.md`
- 문서 분류 규칙 추가 (걸치는 문서 → 양쪽 복사)
- 워크플로우 규칙의 문서 경로 갱신

**Step 2: 메모리 파일 수정**

- `MEMORY.md`의 모노레포 구조 섹션 갱신 (apex_docs → docs)

**Step 3: 커밋**

```bash
git add CLAUDE.md
git commit -m "docs: CLAUDE.md 경로 업데이트 — apex_docs → docs 통합 반영"
```

---

### Task 9: 최종 검증 + 정리

**Step 1: 전체 구조 검증**

```bash
# apex_core/docs/ 가 존재하지 않는지 확인
test ! -d apex_core/docs && echo "OK: apex_core/docs removed"

# apex_docs/ 가 존재하지 않는지 확인
test ! -d apex_docs && echo "OK: apex_docs removed"

# docs/ 구조 확인
find docs/ -type d | sort

# 파일 수 확인 (설계 기준: ~51건 + 복사 1건 + 설계/구현 계획 2건 = ~54건)
find docs/ -type f -name "*.md" | wc -l
```

**Step 2: git log 확인**

```bash
git log --oneline
```
