# BACKLOG 리팩토링 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BACKLOG.md를 2축 분류 체계로 리팩토링하고, 전체 문서와 구현 간의 정합성을 확보한다.

**Architecture:** 기존 BACKLOG.md를 시간축(NOW/IN VIEW/DEFERRED) × 내용축(CRITICAL/MAJOR/MINOR) 2축 체계로 재구성. 히스토리 문서 신설. 별도 백로그 2건 통합. 전 레이어 문서-구현 정합성 검증.

**Spec:** `docs/apex_common/plans/20260318_122201_backlog-refactoring-design.md`

**Prerequisites:** `feature/backlog_refactoring` 브랜치에서 작업 (이미 생성됨)

**공통 규칙:** 모든 커밋 스텝은 커밋 후 `git push`를 포함한다 (프로젝트 규칙: "커밋 즉시 리모트 푸시").

---

## Task 1: 템플릿 파일 생성

새 BACKLOG.md 빈 구조 + BACKLOG_HISTORY.md 생성.

**Files:**
- Modify: `docs/BACKLOG.md` (전체 교체)
- Create: `docs/BACKLOG_HISTORY.md`

- [ ] **Step 1: 기존 BACKLOG.md 백업 읽기**

`docs/BACKLOG.md` 전문을 읽어 기존 항목 목록 파악. Task 4에서 마이그레이션 시 원본으로 사용.

- [ ] **Step 2: 새 BACKLOG.md 작성**

기존 내용을 전부 비우고 새 구조로 교체:

```markdown
# BACKLOG

미해결 이슈 집약. 새 작업 시작 전 반드시 확인.
완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록.
운영 규칙: `docs/CLAUDE.md` § 백로그 운영 참조.

다음 발번: 1

---

## NOW

(마이그레이션 후 항목 배치)

---

## IN VIEW

(마이그레이션 후 항목 배치)

---

## DEFERRED

(마이그레이션 후 항목 배치)
```

- [ ] **Step 3: BACKLOG_HISTORY.md 생성**

```markdown
# BACKLOG HISTORY

완료된 백로그 항목 아카이브. 최신 항목이 파일 상단.

<!-- NEW_ENTRY_BELOW -->

---
```

- [ ] **Step 4: 커밋**

```bash
git add docs/BACKLOG.md docs/BACKLOG_HISTORY.md
git commit -m "docs(backlog): 2축 분류 체계 템플릿 생성 + 히스토리 문서 신설"
```

---

## Task 2: docs/CLAUDE.md 가이드라인 추가

백로그 운영 규칙 섹션을 `docs/CLAUDE.md`에 신설. 설계서 §2~§5 내용을 자립형으로 수록.

**Files:**
- Modify: `docs/CLAUDE.md`

- [ ] **Step 1: docs/CLAUDE.md 읽기**

현재 내용 확인. 기존 "BACKLOG 항목 완료 시 해당 항목 즉시 삭제 (git이 이력 보존)" 라인 위치 파악.

- [ ] **Step 2: 기존 규칙 교체**

`docs/CLAUDE.md` line 19의 기존 규칙:
```
- BACKLOG 항목 완료 시 해당 항목 즉시 삭제 (git이 이력 보존)
```
→ 교체:
```
- BACKLOG 항목 완료 시 해당 항목 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록
```

- [ ] **Step 3: 백로그 운영 섹션 추가**

`docs/CLAUDE.md` 파일 끝에 다음 섹션 추가. 내용은 설계서 §2~§5를 자립형으로 수록:

```markdown
## 백로그 운영

### 2축 분류 체계

**시간축 (1차 분류)** — 문서 내 섹션 순서: `## NOW` → `## IN VIEW` → `## DEFERRED`

| 등급 | 판단 질문 | 기준 |
|------|-----------|------|
| **NOW** | 이번 마일스톤에서 안 하면 다음 단계 진행이 막히는가? | 현재 마일스톤의 선행 조건/블로커. 이미 착수 결정된 항목 |
| **IN VIEW** | 다음 1-2 마일스톤 내에 필요해질 게 확실한가? | 가까운 미래에 필요. 구체적 트리거 조건 있음 |
| **DEFERRED** | 위 둘 다 아닌 경우 | 외부 조건 충족 시 재평가 |

승격 규칙: 마일스톤 전환 시 IN VIEW 재평가 → NOW 승격 여부 판단. DEFERRED는 트리거 조건 발생 시에만 재평가.

**내용축 (2차 분류)** — 각 항목의 `등급` 필드

| 등급 | 판단 질문 | 기준 |
|------|-----------|------|
| **CRITICAL** | 안 고치면 시스템이 깨지거나 데이터가 위험한가? | 크래시, 데이터 손실/손상, 보안 취약점 |
| **MAJOR** | 안 고치면 이후 작업 비용이 눈에 띄게 늘어나는가? | 설계 부채, 테스트 부재, 문서 부재, 확장성 병목 |
| **MINOR** | 위 둘 다 아닌 경우 | 기능·안정성에 영향 없음. 코드 위생, 오타, 탐색적 최적화 |

경계 케이스: 고민되면 높은 쪽으로.

### 항목 템플릿

```
### #{ID}. 이슈 제목
- **등급**: CRITICAL | MAJOR | MINOR
- **스코프**: {모듈 태그, 복수 가능}
- **타입**: bug | design-debt | test | docs | perf | security | infra
- **설명**: 이슈 상세.
```

**스코프 태그**: `core | shared | gateway | auth-svc | chat-svc | infra | ci | docs | tools` (서비스 추가 시 확장)
**타입 태그**: `bug | design-debt | test | docs | perf | security | infra`

### 항목 ID

- Auto-increment 정수. 자릿수 패딩 없음 (`#1`, `#12`, `#100`)
- 한 번 발번된 ID는 재사용하지 않음. ID 갭 허용
- `다음 발번` 카운터를 `docs/BACKLOG.md` 헤더에서 관리
- 머지 충돌 시: 높은 쪽 번호 + 1로 재발번

### 접근 규칙

BACKLOG 항목을 추가·수정·착수·리뷰할 때, 반드시 실제 구현 상태(코드베이스, git 이력, 테스트 결과 등)를 검증하여 사실관계를 확인한다. 문서에 적힌 상태를 그대로 신뢰하지 않는다.

### 히스토리 운영 (`docs/BACKLOG_HISTORY.md`)

- 완료된 항목은 BACKLOG.md에서 삭제 → BACKLOG_HISTORY.md에 prepend
- 삽입 위치: `<!-- NEW_ENTRY_BELOW -->` 마커 바로 아래. 마커 누락 시 파일 상단(헤더 직후)에 복원 후 작업
- 해결 시점: `YYYY-MM-DD HH:MM:SS` 형식
- 커밋 필드: 선택 (해당 없는 경우 생략)
- 해결 방식: FIXED(코드 수정) | DOCUMENTED(문서/주석 보강) | WONTFIX(수정 불필요) | DUPLICATE(중복, 비고에 대상) | SUPERSEDED(다른 작업으로 해소)

히스토리 항목 템플릿:
```
### #{ID}. 이슈 제목
- **등급**: {등급} | **스코프**: {스코프} | **타입**: {타입}
- **해결**: YYYY-MM-DD HH:MM:SS | **방식**: {방식} | **커밋**: {short hash, 선택}
- **비고**: {특이사항 또는 —}
```

- [ ] **Step 4: 커밋**

```bash
git add docs/CLAUDE.md
git commit -m "docs(rules): 백로그 2축 분류 체계 운영 가이드라인 추가"
```

---

## Task 3: 루트 CLAUDE.md 업데이트

루트 `CLAUDE.md`의 백로그 관련 규칙을 신규 체계에 맞게 교체.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: CLAUDE.md 읽기**

루트 `CLAUDE.md` line 50 부근의 기존 백로그 규칙 확인.

- [ ] **Step 2: 백로그 규칙 교체**

기존:
```
- **백로그**: `docs/BACKLOG.md`에 기록. 별도 백로그 파일 생성 금지. 완료 항목은 즉시 삭제 (git이 이력 보존)
```
→ 교체:
```
- **백로그**: `docs/BACKLOG.md`에 기록 (2축 분류: NOW/IN VIEW/DEFERRED × CRITICAL/MAJOR/MINOR). 별도 백로그 파일 생성 금지. 완료 항목은 즉시 삭제 후 `docs/BACKLOG_HISTORY.md`에 기록. 운영 규칙 상세: `docs/CLAUDE.md` § 백로그 운영
```

- [ ] **Step 3: 커밋**

```bash
git add CLAUDE.md
git commit -m "docs(rules): 루트 CLAUDE.md 백로그 규칙을 2축 체계로 교체"
```

---

## Task 4: 레이어 A — BACKLOG 항목 마이그레이션 + 유효성 검증

핵심 작업. 3개 소스 파일의 모든 항목을 코드베이스와 대조 검증 후 새 포맷으로 마이그레이션.

**Files:**
- Modify: `docs/BACKLOG.md`
- Modify: `docs/BACKLOG_HISTORY.md`
- Delete: `docs/apex_core/backlog_memory_os_level.md`
- Delete: `docs/apex_shared/review/20260315_094300_backlog.md`

### Phase 4-1: 서브에이전트 병렬 검증

- [ ] **Step 1: 3개 서브에이전트 동시 디스패치**

각 서브에이전트에게 소스 파일 하나씩 배정. 지시 사항:
- 설계서의 매핑 가이드(§6.1)를 참조하여 각 항목에 대해:
  - 코드베이스/git에서 이슈가 아직 존재하는지 검증
  - 현재 상태(미구현/부분구현/완료/해당없음) 판정
  - 신규 2축 등급 제안: 시간축(NOW/IN VIEW/DEFERRED) + 내용축(CRITICAL/MAJOR/MINOR)
  - 스코프 태그, 타입 태그 제안
  - 설명 텍스트 초안 (파일 경로 등 휘발성 정보는 설명에 포함)
- **파일 수정 금지** — 검증 결과만 보고

**서브에이전트 A**: `docs/BACKLOG.md` 기존 ~30개 항목 검증
**서브에이전트 B**: `docs/apex_core/backlog_memory_os_level.md` 5개 항목 검증 (3개 중복 주의)
**서브에이전트 C**: `docs/apex_shared/review/20260315_094300_backlog.md` ~19개 항목 검증

- [ ] **Step 2: 결과 수집 및 중복 해소**

3개 서브에이전트 결과를 취합하여:
- 중복 항목 식별 → 더 상세한 기술을 택하여 단일 항목으로 통합
- "해당 없음" 항목 → BACKLOG_HISTORY.md에 WONTFIX로 분류
- 이미 해결된 항목 → BACKLOG_HISTORY.md에 FIXED 또는 SUPERSEDED로 분류

### Phase 4-2: 메인이 순차 기록

- [ ] **Step 3: BACKLOG.md에 검증된 항목 기록**

검증 결과를 바탕으로 각 항목을 새 포맷에 맞게 BACKLOG.md에 순차 작성:
- NOW 섹션: 현재 마일스톤 블로커/선행 조건
- IN VIEW 섹션: 가까운 미래 필요 항목
- DEFERRED 섹션: 조건부 재평가 항목
- `다음 발번` 카운터를 최종 ID + 1로 갱신

- [ ] **Step 4: BACKLOG_HISTORY.md에 아카이브 항목 기록**

해결/해당없음/중복 항목을 히스토리에 prepend. `<!-- NEW_ENTRY_BELOW -->` 마커 아래에 삽입.

- [ ] **Step 5: 별도 백로그 파일 삭제 + 커밋**

`git rm`은 삭제와 동시에 staging하므로, BACKLOG 파일들과 함께 한 번에 커밋:

```bash
git rm docs/apex_core/backlog_memory_os_level.md
git rm docs/apex_shared/review/20260315_094300_backlog.md
git add docs/BACKLOG.md docs/BACKLOG_HISTORY.md
git commit -m "docs(backlog): 전 항목 2축 체계 마이그레이션 + 유효성 검증 완료"
git push
```

---

## Task 5: 레이어 B — Apex_Pipeline.md 로드맵 정합성

**Files:**
- Modify: `docs/Apex_Pipeline.md` (필요 시)

- [ ] **Step 1: Apex_Pipeline.md §9, §10 읽기**

버전 이력(§9)과 로드맵(§10) 섹션 확인.

- [ ] **Step 2: 현재 main 상태와 대조**

- v0.5.5.1 기술 내용이 실제와 일치하는지 (71/71 유닛, 11/11 E2E, 구현 항목 등)
- 로드맵에 기술된 v0.6 계획이 새 BACKLOG.md NOW 항목과 모순 없는지
- git log로 최근 변경사항 확인

- [ ] **Step 3: 불일치 수정**

발견된 불일치를 Apex_Pipeline.md에서 수정.

- [ ] **Step 4: 커밋 (변경이 있는 경우만)**

```bash
git add docs/Apex_Pipeline.md
git commit -m "docs(roadmap): Apex_Pipeline.md 로드맵 정합성 수정"
```

---

## Task 6: 레이어 C — plans/progress/review 추적성

**Files:**
- 대상: `docs/` 하위 전체 plans/progress/review 디렉토리

- [ ] **Step 1: plans → progress 매핑 전수 조사**

서브에이전트를 사용하여 `docs/*/plans/` 내 모든 파일과 `docs/*/progress/` 내 모든 파일을 대조.
- 각 plans 문서에 대응하는 progress 존재 여부 확인
- 대응 없는 plans 식별

- [ ] **Step 2: 갭 분석 및 처리**

- 해결 가능한 갭: progress 문서 생성 또는 기존 문서에 추적 정보 추가
- 복원 불가능한 갭(원본 데이터 부재): 비고로 기록 — 이미 BACKLOG에 "plans-progress 추적성 갭 2건" (I-4 관련)이 있으므로 검증 결과에 따라 처리
- 결과를 BACKLOG.md에 반영 (해결된 항목은 히스토리로 이동)

- [ ] **Step 3: 커밋 (변경이 있는 경우만)**

```bash
git add docs/
git commit -m "docs: plans/progress/review 추적성 갭 수정"
```

---

## Task 7: 레이어 D — CLAUDE.md 최종 정합성

**Files:**
- Modify: `CLAUDE.md` (루트)
- Modify: `docs/CLAUDE.md`
- Verify: `apex_core/CLAUDE.md`, `apex_tools/CLAUDE.md`, `apex_services/tests/e2e/CLAUDE.md`, `.github/CLAUDE.md`

- [ ] **Step 1: 전체 CLAUDE.md 파일 목록 확인**

```bash
find . -name "CLAUDE.md" -type f
```

- [ ] **Step 2: 각 CLAUDE.md 검증**

서브에이전트를 사용하여 각 CLAUDE.md를 현재 프로젝트 구조/규칙과 대조:
- Task 2~3에서 추가한 백로그 규칙이 올바르게 반영되었는지
- 기존 규칙 중 현실과 안 맞는 게 있는지 (파일 경로, 빌드 명령, 프로젝트 구조 등)
- 규칙 간 모순 없는지

- [ ] **Step 3: 불일치 수정**

발견된 불일치를 각 CLAUDE.md에서 수정.

- [ ] **Step 4: 커밋 (변경이 있는 경우만)**

Step 3에서 수정한 파일을 명시적으로 지정:

```bash
git add CLAUDE.md docs/CLAUDE.md  # + Step 3에서 수정된 추가 CLAUDE.md 파일들
git commit -m "docs(rules): 전체 CLAUDE.md 규칙 정합성 수정"
git push
```

---

## Task 8: 최종 검증 + progress 문서

**Files:**
- Create: `docs/apex_common/progress/YYYYMMDD_HHMMSS_backlog-refactoring.md`

- [ ] **Step 1: BACKLOG.md 최종 상태 검증**

- 모든 항목이 새 포맷을 따르는지
- 다음 발번 카운터가 정확한지
- NOW/IN VIEW/DEFERRED 배치가 적절한지

- [ ] **Step 2: BACKLOG_HISTORY.md 검증**

- 마커가 올바른 위치에 있는지
- 아카이브된 항목 포맷이 정확한지

- [ ] **Step 3: 머지 전 필수 갱신 확인**

프로젝트 규칙에 따라 머지 전 갱신 대상 확인:
- `docs/Apex_Pipeline.md` — Task 5에서 처리 완료 확인
- `CLAUDE.md` 로드맵 — 이번 작업은 docs-only이므로 버전 변경 없음. 변경 불필요 확인
- `README.md` — 백로그 체계 변경이 README에 영향 주는지 확인, 필요 시 갱신
- `docs/BACKLOG.md` — Task 4에서 처리 완료 확인

- [ ] **Step 4: progress 문서 작성**

작업 결과 요약:
- 마이그레이션 통계 (총 항목 수, NOW/IN VIEW/DEFERRED 분포, 히스토리 이동 건수)
- 정합성 검증 결과 요약 (레이어별)
- TODO/백로그는 BACKLOG.md로 이전 — progress 문서에 잔류 금지

- [ ] **Step 5: 최종 커밋**

```bash
git add docs/apex_common/progress/
git commit -m "docs(progress): BACKLOG 리팩토링 완료 기록"
git push
```
