# apex_tools — 도구 & 플러그인 가이드

## 로컬 플러그인 캐시 트러블슈팅

스킬 invoke 시 실제 파일과 다른 (오래된) 내용이 로드되면 **플러그인 캐시 문제**. 세션 재시작으로는 해결 안 됨.

**진단**: 스킬 invoke 결과와 실제 파일(`apex_tools/claude-plugin/commands/*.md`) diff 비교

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

- 태스크 완료 후 `/auto-review task` 묻지 말고 자동 실행 — 리뷰 → 수정 → 재리뷰 → Clean → PR+CI 전 과정 자동화
- coordinator/리뷰어가 정의된 프로세스대로 동작 중이면 재촉하지 않고 기다림. 프로세스에 report 전송이 명시되어 있으면 요청 없이 자동 전송될 때까지 대기
- **스킬 수정 후 캐시 삭제 필수**: 프롬프트, 에이전트 정의 등 auto-review 스킬을 수정하면 플러그인 캐시를 삭제해야 변경사항이 반영됨
  ```bash
  rm -rf ~/.claude/plugins/cache/apex-local/apex-auto-review/
  ```
  삭제 후 Claude Code 재시작 또는 새 세션에서 적용 확인

## 빌드 관련

- **빌드는 항상 `run_in_background: true`로 실행** — `timeout` 파라미터 절대 설정 금지. 완료 알림까지 무한 대기
- **빌드는 한 번에 하나만** — MSVC+Ninja가 멀티코어를 풀로 사용하므로 동시 빌드 시 시스템 렉
