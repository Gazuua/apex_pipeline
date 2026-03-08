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

## 기타

- `new-service.sh` — 서비스 스캐폴딩 스크립트 (Phase 9에서 추가)
