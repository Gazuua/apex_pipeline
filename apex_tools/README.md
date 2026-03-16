# apex_tools

개발 도구 및 스크립트.

## Git Hooks

프로젝트 공용 Git hook 스크립트. 클론 후 한 번만 실행하면 적용됨:

```bash
git config core.hooksPath apex_tools/git-hooks
```

| Hook | 설명 |
|------|------|
| `pre-commit` | main 직접 커밋 차단 (squash merge는 허용) |

## Session Context 훅

세션 시작 시 프로젝트 컨텍스트(README.md + git 상태)를 자동 주입하는 `SessionStart` 훅.
Claude가 매 세션마다 별도 질문 없이 프로젝트 현황을 파악한 상태로 시작함.

| 항목 | 내용 |
|------|------|
| 스크립트 | `session-context.sh` |
| 출력 | README.md 전문 + 현재 브랜치 + `git status --short` + 최근 커밋 5개 |
| 등록 | `.claude/settings.json` → `hooks.SessionStart` |

## Auto-Review 플러그인

자동 리뷰 플러그인. 7개 전문 리뷰어가 병렬로 코드를 검사하고, 이슈 수정 → 재리뷰를 Clean(0건)까지 반복한 뒤 PR 생성 + CI 통과까지 처리.

### 셋업

Claude Code 세션 시작 시 `SessionStart` 훅이 자동으로 플러그인을 등록함. 별도 설치 불필요.

수동 설치가 필요한 경우:

```bash
bash apex_tools/setup-claude-plugin.sh
```

### 사용법

```
/auto-review task   # git diff 기반 — 변경 파일만 리뷰
/auto-review full   # 전체 프로젝트 리뷰
```

### 리뷰어 에이전트

| 에이전트 | 담당 |
|---------|------|
| `reviewer-docs-spec` | 원천 문서 정합성 (설계서/README/CLAUDE.md 간 버전/용어/로드맵 일치) |
| `reviewer-docs-records` | 기록 문서 형식/완결성 (plans/progress/review) |
| `reviewer-design` | 설계/API/아키텍처 정합 (아키텍처 패턴, 모듈 경계, 인터페이스 일관성) |
| `reviewer-logic` | 비즈니스 로직/알고리즘 정확성 |
| `reviewer-systems` | 메모리/동시성/저수준 (할당기, 락프리, 코루틴, MPSC) |
| `reviewer-test` | 테스트 커버리지+품질 (누락 경로, 격리, assertion) |
| `reviewer-infra-security` | 빌드/CI/인프라/보안 (CMake, Docker, CI/CD, 입력 검증, 인증) |

### 플러그인 구조

```
apex_tools/
├── .claude-plugin/marketplace.json       ← 로컬 마켓플레이스
├── setup-claude-plugin.sh                ← 자동 셋업 스크립트
├── session-context.sh                    ← 세션 컨텍스트 자동 주입
├── auto-review/config.md                 ← auto-review 설정
└── claude-plugin/
    ├── commands/auto-review.md           ← 오케스트레이터
    └── agents/
        └── reviewer-{docs-spec,docs-records,design,logic,systems,test,infra-security}.md
```

## 기타

- `new-service.sh` — 서비스 스캐폴딩 스크립트 (v1.0.0.0에서 추가)
