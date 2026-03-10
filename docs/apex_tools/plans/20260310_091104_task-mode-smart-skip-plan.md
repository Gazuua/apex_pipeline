# Task 모드 Round 1 스마트 스킵 구현 계획

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** auto-review task 모드 Phase 1 Round 1에서 변경 파일 타입 기반 에이전트 스마트 스킵 적용

**Architecture:** `auto-review.md` 단일 파일 수정. Phase 2의 파일타입 매핑 테이블을 공용 섹션으로 승격하고 확장, Phase 1에 task 모드 Round 1 스킵 로직 추가, Phase 2는 공용 매핑 참조로 변경.

**Tech Stack:** Markdown (Claude plugin command spec)

---

## Task 1: 공용 파일타입 매핑 테이블 섹션 추가

**Files:**
- Modify: `apex_tools/claude-plugin/commands/auto-review.md:17` (Phase 0과 Phase 1 사이에 삽입)

- [ ] **Step 1: Phase 0 뒤(라인 34 `---` 뒤)에 공용 매핑 섹션 삽입**

`---` (Phase 0 끝)과 `## Phase 1` 사이에 다음 섹션을 삽입:

```markdown
## 공용: 파일타입 → 리뷰어 매핑

Phase 1 task 모드 스마트 스킵과 Phase 2 재리뷰 스마트 스킵이 공유하는 매핑 테이블.

| 수정 파일 타입 | 영향받는 리뷰어 |
|---|---|
| `.cpp`, `.hpp` (소스/헤더) | code, test, structure |
| `test_*.cpp`, `test_helpers.hpp` | test |
| `.fbs` (FlatBuffers 스키마) | code, structure |
| `CMakeLists.txt`, `vcpkg.json`, `build.*`, `CMakePresets*` | structure, general |
| `Dockerfile`, `.dockerignore`, `docker-compose.yml` | structure, general |
| `.github/workflows/*.yml` (CI) | general |
| `*suppressions.txt` (TSAN/LSAN) | general, test |
| `.toml`, `.sql` (설정/DB) | general |
| `.clangd`, `.gitattributes`, `.editorconfig` | general |
| `*.md` (문서) | docs |
| `.gitignore`, hooks, scripts (`.sh`, `.bat`) | general |

**폴백**: 위 매핑에 해당하지 않는 파일이 있으면 → `general` 자동 포함

**의미적 영향 추가 판단** (오케스트레이터 자율):
- 코드 로직 변경 (if/for/while 등 제어 흐름) → docs 추가 (설계 정합성 영향)
- 의존성 변경 (vcpkg, include) → general 추가
- 디렉토리/파일 구조 변경 → 전원 재리뷰
- **판단이 애매하면 → 해당 리뷰어를 포함** (보수적으로)
```

---

## Task 2: Phase 1 Round 1 스마트 스킵 분기 추가

**Files:**
- Modify: `apex_tools/claude-plugin/commands/auto-review.md` Phase 1 섹션 (라인 42-46)

- [ ] **Step 1: 리뷰어 디스패치 규칙을 모드별로 분기**

기존 (라인 42-46):
```markdown
2. **리뷰어 에이전트 병렬 디스패치**

   Agent 도구로 **참여 대상 에이전트를 동시에** 하나의 메시지에서 호출:
   - Round 1: 5개 전원 디스패치
   - Round 2+: Phase 2 Smart Re-review 스킵 판단에 따라 참여 에이전트만 디스패치
```

변경:
```markdown
2. **리뷰어 에이전트 병렬 디스패치**

   Agent 도구로 **참여 대상 에이전트를 동시에** 하나의 메시지에서 호출:

   **Round 1 디스패치 규칙:**
   - `full` 모드: 5개 전원 디스패치
   - `task` 모드: 변경 파일 목록에 **공용 파일타입 매핑**을 적용하여 필요한 리뷰어만 디스패치
     - 변경 파일에서 매핑 테이블로 영향받는 리뷰어 집합을 결정
     - 의미적 영향 추가 판단도 동일하게 적용
     - 어떤 리뷰어도 매핑되지 않는 파일이 있으면 `general` 자동 포함

   **Round 2+ 디스패치 규칙:** (모드 무관)
   - Phase 2 Smart Re-review 스킵 판단에 따라 참여 에이전트만 디스패치
```

---

## Task 3: Phase 1 리포트에 참여 현황 포맷 추가

**Files:**
- Modify: `apex_tools/claude-plugin/commands/auto-review.md` Phase 1 결과 취합 섹션 (라인 61-80)

- [ ] **Step 1: 리뷰 보고서 템플릿에 참여 현황 섹션 추가**

Phase 1의 "결과 취합" 보고서 템플릿(라인 63-78)에 참여 현황 블록 추가:

```markdown
   ## Round {N} 참여 현황
   - 변경 파일: {파일 목록} (task 모드만)
   - reviewer-docs: {리뷰 수행|스킵 (변경 파일 무관)}
   - reviewer-structure: {리뷰 수행|스킵 (변경 파일 무관)}
   - reviewer-code: {리뷰 수행|스킵 (변경 파일 무관)}
   - reviewer-test: {리뷰 수행|스킵 (변경 파일 무관)}
   - reviewer-general: {리뷰 수행|기본 포함|스킵}
```

이 섹션은 `## 요약` 앞에 배치.

---

## Task 4: Phase 2 매핑 테이블을 공용 참조로 교체

**Files:**
- Modify: `apex_tools/claude-plugin/commands/auto-review.md` Phase 2 섹션 (라인 110-124)

- [ ] **Step 1: Phase 2의 인라인 매핑 테이블을 공용 섹션 참조로 교체**

기존 (라인 110-124):
```markdown
   b) 파일 타입 매핑으로 "최소 필수 리뷰어" 결정:

   | 수정 파일 타입 | 영향받는 리뷰어 |
   |--------------|--------------|
   | `.cpp`, `.hpp` (소스/헤더) | code, test, structure |
   | `test_*.cpp`, `test_helpers.hpp` | test |
   | `CMakeLists.txt`, `vcpkg.json`, `build.*`, `CMakePresets*` | structure, general |
   | `*.md` (문서) | docs |
   | `.gitignore`, hooks, scripts | general |

   c) 수정 내용의 **의미적 영향** 추가 판단 (오케스트레이터 자율):
   - 코드 로직 변경 (if/for/while 등 제어 흐름) → docs 추가 (설계 정합성 영향)
   - 의존성 변경 (vcpkg, include) → general 추가
   - 디렉토리/파일 구조 변경 → 전원 재리뷰
   - **판단이 애매하면 → 해당 리뷰어를 포함** (보수적으로)
```

변경:
```markdown
   b) **공용 파일타입 매핑** (위 섹션 참조)으로 "최소 필수 리뷰어" 결정
      - 폴백 및 의미적 영향 판단도 동일 적용
```

---

## Task 5: 커밋

- [ ] **Step 1: 변경 사항 커밋**

```bash
git add apex_tools/claude-plugin/commands/auto-review.md
git commit -m "feat(auto-review): task 모드 Round 1 스마트 스킵 + 파일타입 매핑 확장"
```
