# apex_tools — 도구 & 플러그인 가이드

## 로컬 플러그인 캐시 트러블슈팅

스킬 invoke 시 실제 파일과 다른 (오래된) 내용이 로드되면 **플러그인 캐시 문제**. 세션 재시작으로는 해결 안 됨.

**진단**: 스킬 invoke 결과와 실제 파일(`apex_tools/apex-auto-review/commands/*.md`) diff 비교

**수정 절차:**
```bash
# 1. 캐시 삭제
rm -rf ~/.claude/plugins/cache/apex-local/apex-auto-review/

# 2. ~/.claude/plugins/installed_plugins.json 확인+수정
#    - installPath가 실제 경로와 일치하는지 (오타 주의)
#    - version이 plugin.json과 일치하는지

# 3. 세션 재시작 → 최신 스킬 로드 확인
```

## auto-review

- 메인이 auto-review 필요 여부를 자체 판단하여 실행한다 (유저에게 묻지 않음)
- **BACKLOG_HISTORY 사전 확인 필수**: 리뷰 시작 전 `backlog list --status RESOLVED` CLI로 이미 처리된 이력을 확인하여, 이미 처리된 이력이 있는 이슈는 보고하지 않는다. 중복 보고는 노이즈일 뿐이다 (validate-backlog hook이 파일 직접 접근을 차단하므로 CLI 사용)
- **스킬 수정 후 캐시 삭제 필수**: 프롬프트, 에이전트 정의 등 auto-review 스킬을 수정하면 플러그인 캐시를 삭제해야 변경사항이 반영됨
  ```bash
  rm -rf ~/.claude/plugins/cache/apex-local/apex-auto-review/
  ```
  삭제 후 Claude Code 재시작 또는 새 세션에서 적용 확인
