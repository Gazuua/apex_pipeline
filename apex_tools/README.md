# apex_tools

개발 도구 및 자동화.

## apex-agent (Go 백엔드)

빌드 큐, 핸드오프, 백로그 관리, Hook 게이트, 브랜치 정리 등 프로젝트 자동화를 담당하는 Go 단일 바이너리.
상세: `apex-agent/CLAUDE.md`

### 주요 커맨드

| 커맨드 | 설명 |
|--------|------|
| `apex-agent queue build <preset>` | 빌드 잠금 획득 후 빌드 실행 |
| `apex-agent handoff notify start` | 브랜치 핸드오프 착수 |
| `apex-agent handoff status` | 핸드오프 상태 조회 |
| `apex-agent backlog list` | 백로그 조회 |
| `apex-agent cleanup --execute` | 머지 완료 브랜치 정리 |
| `apex-agent daemon start` | 백그라운드 데몬 시작 |

### Hook 게이트

`.claude/settings.json`에 PreToolUse로 등록. 모두 `run-hook` 래퍼 경유:

| Hook | 역할 |
|------|------|
| `validate-build` | cmake/ninja/build.bat 직접 호출 차단 |
| `validate-merge` | merge lock 미획득 시 `gh pr merge` 차단 |
| `validate-handoff` | 미등록 커밋 차단 + 머지 시 FIXING 백로그 차단 |
| `enforce-rebase` | push 전 자동 리베이스 |
| `validate-backlog` | docs/BACKLOG.json 직접 접근 차단 (Read/Edit/Write 모두) |
| `handoff-probe` | 미등록 편집 차단 + 상태별 소스 게이트 |

## Git Hooks

프로젝트 공용 Git hook 스크립트. 클론 후 한 번만 실행하면 적용됨:

```bash
git config core.hooksPath apex_tools/git-hooks
```

| Hook | 설명 |
|------|------|
| `pre-commit` | main 직접 커밋 차단 (squash merge는 허용) |
| `post-merge` | apex-agent 자동 빌드 트리거 |

## Auto-Review 플러그인

7개 전문 리뷰어가 병렬로 코드를 검사하고, 이슈 수정 → 재리뷰를 Clean(0건)까지 반복.
SessionStart hook이 세션 시작 시 자동으로 플러그인 등록 + 데몬 기동.

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
├── apex-agent/                              ← Go 바이너리 (빌드큐, 핸드오프, 백로그, hook, cleanup)
│   ├── run-hook                             ← 크로스플랫폼 hook 래퍼
│   ├── cmd/apex-agent/main.go               ← 엔트리포인트
│   ├── internal/                            ← 모듈별 구현
│   └── e2e/                                 ← E2E 통합 테스트
├── git-hooks/                               ← Git hook (pre-commit, post-merge)
├── auto-review/config.md                    ← auto-review 설정
└── claude-plugin/
    ├── commands/auto-review.md              ← 오케스트레이터
    └── agents/
        └── reviewer-{docs-spec,...}.md      ← 7개 리뷰어 에이전트
```

## 기타

- `build-preflight.sh` — 빌드 사전 체크 (cmake/ninja/gcc 버전 검증). `build.sh`에서 source
- `new-service.sh` — 서비스 스캐폴딩 스크립트 (v1.0.0.0에서 추가 예정)
