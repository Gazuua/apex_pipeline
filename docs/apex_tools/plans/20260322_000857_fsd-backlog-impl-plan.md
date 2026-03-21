# FSD Backlog 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `/fsd-backlog` 슬래시 커맨드를 구현하여 백로그 소탕을 완전 자율 수행할 수 있게 한다.

**Architecture:** 기존 `apex_tools/claude-plugin/`에 스킬 프롬프트 파일 1개를 추가하고, plugin.json을 갱신하여 커맨드를 등록한다. 새 코드(C++ 등)는 없으며, 기존 인프라(branch-handoff, queue-lock, auto-review)를 그대로 활용한다.

**Tech Stack:** Claude Code 플러그인 (Markdown 스킬 프롬프트 + JSON 매니페스트)

**Spec:** `docs/apex_tools/plans/20260321_235653_fsd-backlog-design.md`

---

## 파일 구조

| 파일 | 동작 | 역할 |
|------|------|------|
| `apex_tools/claude-plugin/commands/fsd-backlog.md` | 생성 | 슬래시 커맨드 스킬 프롬프트 |
| `apex_tools/claude-plugin/.claude-plugin/plugin.json` | 수정 | 플러그인명·설명 갱신 + 커맨드 자동 검색 유지 |

---

### Task 1: 슬래시 커맨드 스킬 프롬프트 작성

**Files:**
- Create: `apex_tools/claude-plugin/commands/fsd-backlog.md`

- [ ] **Step 1: 스킬 프롬프트 파일 작성**

frontmatter + 소탕 워크플로우 지시 + 핸드오프 사용 가이드를 포함한 스킬 프롬프트 작성.
설계 문서의 "스킬 프롬프트 구조" 섹션을 기반으로 구현.

- [ ] **Step 2: 커밋**

```bash
git add apex_tools/claude-plugin/commands/fsd-backlog.md
git commit -m "feat(tools): BACKLOG-123 /fsd-backlog 슬래시 커맨드 스킬 프롬프트 추가"
```

### Task 2: plugin.json 갱신

**Files:**
- Modify: `apex_tools/claude-plugin/.claude-plugin/plugin.json`

- [ ] **Step 1: plugin.json 갱신**

플러그인명과 설명을 fsd-backlog 커맨드를 포함하도록 갱신.
커맨드는 `commands/` 디렉토리에서 자동 검색되므로 별도 등록은 불필요.

- [ ] **Step 2: 커밋**

```bash
git add apex_tools/claude-plugin/.claude-plugin/plugin.json
git commit -m "chore(tools): BACKLOG-123 plugin.json 설명 갱신"
```

### Task 3: BACKLOG.md 갱신 + progress 문서

**Files:**
- Modify: `docs/BACKLOG.md` — #123 항목 추가 + 다음 발번 갱신
- Modify: `docs/BACKLOG_HISTORY.md` — 완료 시 이전
- Create: `docs/apex_tools/progress/` — 작업 결과 요약

- [ ] **Step 1: BACKLOG.md에 #123 등록 + 완료 처리**
- [ ] **Step 2: progress 문서 작성**
- [ ] **Step 3: 머지 전 필수 문서 갱신 확인**
- [ ] **Step 4: 커밋**
