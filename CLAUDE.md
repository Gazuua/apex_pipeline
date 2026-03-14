# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 모노레포 구조

`apex_core/`(코어), `apex_shared/`(공유 라이브러리+어댑터), `apex_services/`(MSA), `apex_infra/`(Docker/K8s), `apex_tools/`(CLI/스크립트/auto-review), `docs/`(전체 문서 중앙 집중)

## 빌드

- `cmd.exe //c build.bat debug` / `cmd.exe //c build.bat release` (bash 셸에서, `//c` 필수)
- **빌드는 항상 `run_in_background: true`로 실행** — `timeout` 파라미터 절대 설정 금지. 완료 알림까지 무한 대기
- **빌드는 한 번에 하나만** — 동시 빌드 시 시스템 렉
- 상세 (의존성, MSVC 주의사항, 빌드 변형) → `apex_core/CLAUDE.md`

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성
- **현재**: v0.4.5.1 (full auto-review + 문서 정비 + 프로세스 개선)
- **다음**: v0.4 → v0.5 → v0.6 → v1.0.0.0 (프레임워크 완성)
- 상세: `docs/Apex_Pipeline.md` §10

## 전역 규칙

### Git / 브랜치
- **커밋 메시지는 한국어로 작성** — `feat(core): 코루틴 할당기 추가` 형태 (타입+스코프 영어, 설명 한국어)
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **worktree**: `.worktrees/` 하위에 생성 → 직후 `git config --global --add safe.directory D:/.workspace/.worktrees/<name>`
  - 삭제: `git worktree remove` → `git branch -D` → `git push origin --delete` (3점 정리)
- **머지**: 리뷰 이슈 0건 → `gh pr merge --squash --admin --delete-branch` → 워크트리 삭제
- **머지 전 필수 갱신**: `docs/Apex_Pipeline.md`, `CLAUDE.md` 로드맵, `README.md` — 세 문서 최신 반영

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 수정 가능 파일 목록 명시해서 충돌 방지
- **auto-review 프로세스 대기** — coordinator/리뷰어 동작 중이면 재촉하지 않고 완료까지 대기
- **태스크 완료 후 `/auto-review task` 묻지 말고 자동 실행**

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

| 영역 | 파일 |
|------|------|
| 빌드/MSVC/아키텍처/벤치마크 | `apex_core/CLAUDE.md` |
| 문서 작성/리뷰/브레인스토밍 | `docs/CLAUDE.md` |
| 도구/플러그인 캐시/auto-review | `apex_tools/CLAUDE.md` |
| CI/CD 트러블슈팅 | `.github/CLAUDE.md` |
