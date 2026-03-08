# Auto-Review Skill 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** `/auto-review [task|full]` 슬래시 커맨드로 멀티에이전트 리뷰 → 수정 → 재리뷰 루프 → PR → CI 통과까지 자동화

**Architecture:** 프로젝트 로컬 플러그인(apex_tools/claude-plugin/)에 오케스트레이터 커맨드 + 5개 리뷰어 에이전트. pr-review-toolkit 패턴(commands/ + agents/) 준수.

**Tech Stack:** Claude Code Plugin (Markdown), gh CLI, git

**설계서:** `docs/plans/2026-03-08-auto-review-skill-design.md`

---

### Task 1: 플러그인 스캐폴드 생성

**Files:**
- Create: `apex_tools/claude-plugin/.claude-plugin/plugin.json`

**Step 1: 디렉토리 생성**

```bash
mkdir -p apex_tools/claude-plugin/.claude-plugin
mkdir -p apex_tools/claude-plugin/commands
mkdir -p apex_tools/claude-plugin/agents
```

**Step 2: plugin.json 작성**

```json
{
  "name": "apex-auto-review",
  "description": "Automated multi-agent review, fix, and CI pipeline for Apex Pipeline",
  "version": "1.0.0",
  "author": {
    "name": "Hogyun Jung"
  }
}
```

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/
git commit -m "chore: auto-review 플러그인 스캐폴드 생성"
```

---

### Task 2: 오케스트레이터 커맨드 작성

**Files:**
- Create: `apex_tools/claude-plugin/commands/auto-review.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
description: "Automated multi-agent review → fix → re-review loop until clean, then PR + CI"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent"]
---
```

**Step 2: 오케스트레이터 본문 작성**

커맨드 본문에 포함할 내용:
- 인자 파싱 (`task` vs `full`, 기본값 `task`)
- Phase 1: 리뷰 범위 결정 + 5개 리뷰어 에이전트 병렬 디스패치 + 결과 취합 → 종합 보고서
- Phase 2: 이슈 수정 루프 (파일 비중복 병렬 수정 → 재리뷰 → Clean까지)
- Phase 3: PR 생성 (리뷰 보고서 커밋 + `gh pr create` + `gh run watch`)
- Phase 4: CI 수정 루프 (실패 시 로그 분석 → 수정 → 재푸시 → `gh run watch`)
- Phase 5: 완료 보고서 커밋 + 푸시 + 유저 보고
- 안전장치: 동일 이슈 5회 반복 → 중단
- 이슈 추적: `파일경로:카테고리:이슈요약` 키로 라운드별 누적
- 각 리뷰어 에이전트 참조: `agents/reviewer-*.md`

**참조:** `pr-review-toolkit/commands/review-pr.md` 포맷 준수

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/commands/auto-review.md
git commit -m "feat: auto-review 오케스트레이터 커맨드 작성"
```

---

### Task 3: 문서 정합성 리뷰어 에이전트 작성

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-docs.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
name: reviewer-docs
description: "문서 정합성 리뷰 — 마스터 문서, 설계서, README, MEMORY가 프로젝트 최신 상태를 반영하는지 검증"
model: opus
color: blue
---
```

**Step 2: 에이전트 프롬프트 작성**

포함할 내용:
- 역할 정의: 문서와 실제 프로젝트 상태 간 불일치 검출
- 체크 대상: `docs/Apex_Pipeline.md`, `CLAUDE.md`, `README.md`, MEMORY, 설계서(`docs/plans/`), 리뷰 보고서(`docs/*/review/`)
- 심각도 기준 (Critical/Important/Minor)
- 출력 포맷 통일 (파일:라인, 원인, 영향, 수정 방안)
- 자율 병렬 분할 지침: 문서가 많으면 카테고리별 서브에이전트 분할
- task/full 모드 대응

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-docs.md
git commit -m "feat: reviewer-docs 에이전트 프롬프트 작성"
```

---

### Task 4: 구조/구현 정합성 리뷰어 에이전트 작성

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-structure.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
name: reviewer-structure
description: "프로젝트 구조 및 구현 정합성 리뷰 — 파일 배치, 모듈 경계, 의존성이 설계 철학과 일치하는지 검증"
model: opus
color: cyan
---
```

**Step 2: 에이전트 프롬프트 작성**

포함할 내용:
- 역할 정의: 모노레포 구조, CMake 구성, 모듈 경계가 설계 문서와 일치하는지 검증
- 체크 대상: 디렉토리 레이아웃 vs CLAUDE.md 모노레포 구조, CMakeLists.txt 의존성, 헤더/소스 배치 규칙, vcpkg.json
- 심각도 기준 + 출력 포맷 통일
- 자율 병렬 분할 지침: 서브 프로젝트별 분할 (apex_core, apex_shared 등)
- task/full 모드 대응

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-structure.md
git commit -m "feat: reviewer-structure 에이전트 프롬프트 작성"
```

---

### Task 5: 코드 리뷰어 에이전트 작성

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-code.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
name: reviewer-code
description: "코드 품질 리뷰 — 구현 정확성, 버그, 보안, 성능, 설계 패턴 검증"
model: opus
color: green
---
```

**Step 2: 에이전트 프롬프트 작성**

포함할 내용:
- 역할 정의: 코드가 의도대로 구현되었는지, 기술적 결함 검출
- 체크 대상: C++23 코루틴 패턴, Boost.Asio 사용법, MSVC 호환성, RAII/수명 관리, 스레드 안전성, 메모리 안전성
- CLAUDE.md MSVC 주의사항 참조 (aligned_alloc, CRTP, MessageDispatcher)
- Confidence scoring (≥80만 보고)
- 출력 포맷 통일 + 자율 병렬 분할 (모듈/디렉토리 단위)
- task/full 모드 대응

**참조:** `pr-review-toolkit/agents/code-reviewer.md` 포맷

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-code.md
git commit -m "feat: reviewer-code 에이전트 프롬프트 작성"
```

---

### Task 6: 테스트 리뷰어 에이전트 작성

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-test.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
name: reviewer-test
description: "테스트 리뷰 — 커버리지, 누락 테스트, 엣지 케이스, assertion 품질, 테스트 격리 검증"
model: opus
color: yellow
---
```

**Step 2: 에이전트 프롬프트 작성**

포함할 내용:
- 역할 정의: 테스트 코드 품질 및 커버리지 검증
- 체크 대상: GTest 테스트 파일, 테스트 격리(io_context 독립성), assertion 적절성, 엣지 케이스 커버리지, 누락 테스트
- 기존 CI 구성 참조 (ASAN/TSAN/GCC/MSVC 4잡)
- 출력 포맷 통일 + 자율 병렬 분할 (테스트 파일 단위)
- task/full 모드 대응

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-test.md
git commit -m "feat: reviewer-test 에이전트 프롬프트 작성"
```

---

### Task 7: 기타 리뷰어 에이전트 작성

**Files:**
- Create: `apex_tools/claude-plugin/agents/reviewer-general.md`

**Step 1: YAML frontmatter 정의**

```yaml
---
name: reviewer-general
description: "기타 리뷰 — 빌드 시스템, CI/CD, 의존성, 라이선싱 등 다른 리뷰어가 놓칠 수 있는 영역 자율 검증"
model: opus
color: magenta
---
```

**Step 2: 에이전트 프롬프트 작성**

포함할 내용:
- 역할 정의: 다른 4개 리뷰어가 커버하지 않는 영역 자율 판단
- 가능한 체크 대상: CMake/빌드 스크립트, GitHub Actions workflow, vcpkg 의존성 버전, .gitignore, pre-commit hooks, 라이선싱
- 자율적 판단 강조 — 해당 없으면 "이슈 없음" 보고도 OK
- 출력 포맷 통일 + 자율 병렬 분할
- task/full 모드 대응

**Step 3: 커밋**

```bash
git add apex_tools/claude-plugin/agents/reviewer-general.md
git commit -m "feat: reviewer-general 에이전트 프롬프트 작성"
```

---

### Task 8: 플러그인 등록

**Step 1: 프로젝트 설정에 로컬 플러그인 등록**

plugin-dev:plugin-structure 스킬을 참조하여 프로젝트 로컬 플러그인을 Claude Code에 등록.
가능한 방법:
- `.claude/settings.json`에 로컬 경로 추가
- 또는 프로젝트 `.claude.json`에 등록

**Step 2: 등록 확인**

Claude Code 재시작 후 `/auto-review` 커맨드가 인식되는지 확인.

**Step 3: 커밋 (설정 파일 변경 시)**

```bash
git add .claude/ || true
git commit -m "chore: auto-review 플러그인 로컬 등록"
```

---

### Task 9: 스킬 검증

**Step 1: 호출 테스트**

```
/auto-review task
```

기대 결과: 5개 리뷰어 에이전트가 병렬 디스패치되고, 종합 보고서 생성

**Step 2: 문제 수정**

호출 중 문제 발견 시 즉시 수정.

**Step 3: 최종 커밋**

```bash
git add -A
git commit -m "fix: auto-review 스킬 검증 후 수정"
```
