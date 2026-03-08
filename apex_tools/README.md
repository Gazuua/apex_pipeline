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

## Auto-Review 플러그인

멀티에이전트 자동 리뷰 플러그인. 5개 전문 리뷰어가 병렬로 코드를 검사하고, 이슈 수정 → 재리뷰를 Clean(0건)까지 반복한 뒤 PR 생성 + CI 통과까지 처리.

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
| `reviewer-docs` | 문서 정합성 (마스터 설계서, README, MEMORY) |
| `reviewer-structure` | 구조/구현 정합성 (디렉토리, CMake, 모듈 경계) |
| `reviewer-code` | 코드 품질 (버그, 보안, 성능, 설계 패턴) |
| `reviewer-test` | 테스트 (커버리지, 엣지 케이스, 격리, assertion) |
| `reviewer-general` | 기타 (빌드, CI/CD, 의존성, 라이선싱) |

### 플러그인 구조

```
apex_tools/
├── .claude-plugin/marketplace.json       ← 로컬 마켓플레이스
├── setup-claude-plugin.sh                ← 자동 셋업 스크립트
└── claude-plugin/
    ├── .claude-plugin/plugin.json        ← 플러그인 매니페스트
    ├── commands/auto-review.md           ← 오케스트레이터
    └── agents/
        └── reviewer-{docs,structure,code,test,general}.md
```

## 기타

- `new-service.sh` — 서비스 스캐폴딩 스크립트 (Phase 9에서 추가)
