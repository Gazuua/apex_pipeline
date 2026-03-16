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
- **워크트리 빌드 주의사항**:
  - 워크트리에서 `build.bat` 실행 시 `vswhere.exe` 경로 탐색 문제로 실패할 수 있음
  - 첫 빌드 시 vcpkg 패키지 설치로 수 분~수십 분 소요 가능
  - 빌드 실패가 코드 문제가 아닌 환경 문제로 의심되면, 메인 워크스페이스(`D:\.workspace`)에서 검증 권장
- **병렬 에이전트 빌드 조율**: 여러 에이전트 동시 작업 시 빌드는 한 번에 하나만 수행 (동시 빌드 시 시스템 렉)
- **빌드 오류 책임**: 빌드 실패는 작업 프로세스 내에서 해결. auto-review와 별개 — 작업 완료+빌드 성공 확인 후 리뷰 진입
- 상세 (의존성, MSVC 주의사항, 빌드 변형) → `apex_core/CLAUDE.md`

## clangd

- **clangd가 Claude Code에 LSP로 연결**되어 있음 — 별도 에디터 없음
- `.clangd` 설정이 MSVC 플래그를 clang 호환으로 변환 (Remove + Add)
- `compile_commands.json`은 `build.bat`이 빌드 후 루트로 복사
- vcpkg 인클루드 경로: `-external:I` → Remove로 제거되므로 `.clangd` Add에 `-I`로 재추가 필수

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성
- **현재**: v0.5.5 — 서비스 체인 완성 (PR #30 리뷰 8건 + Auth/Chat full impl + E2E 인프라 + 56 테스트)
- **다음**: v0.6 (운영 인프라) → v1.0.0.0 (프레임워크 완성)
- 상세: `docs/Apex_Pipeline.md` §10

## 전역 규칙

### Git / 브랜치
- **커밋 메시지는 한국어로 작성** — `feat(core): 코루틴 할당기 추가` 형태 (타입+스코프 영어, 설명 한국어)
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **worktree**: `.worktrees/` 하위에 생성 → 직후 `git config --global --add safe.directory D:/.workspace/.worktrees/<name>`
  - **생성 직후 빈 커밋 필수**: `git commit --allow-empty -m "chore: 작업 브랜치 생성"` — cleanup 스크립트의 미머지 브랜치 필터에 걸리도록
  - **일괄 정리**: `apex_tools/cleanup-branches.sh` (dry-run 기본, `--execute`로 실행)
- **머지**: 리뷰 이슈 0건 → `gh pr merge --squash --admin`
- **머지 전 필수 갱신**: `docs/Apex_Pipeline.md`, `CLAUDE.md` 로드맵, `README.md`, `docs/BACKLOG.md` — 머지 직전에 갱신하므로 **완료 상태로 기재** (구현 중/리뷰 중이 아님)
- **브랜치 이관 금지**: 작업 시작 브랜치 = PR 브랜치. 중간에 새 브랜치로 이관하지 않음. 불가피하면 새 브랜치 푸시 시점에 `git push origin --delete {원본브랜치}`로 원본 리모트 즉시 삭제 — cleanup 스크립트가 탐지 불가한 고아 브랜치 방지

### 설계 원칙
- **Gateway 서비스 독립성**: Gateway는 개별 서비스의 도메인 지식에 절대 의존 금지. 서비스 추가/변경 시 Gateway 코드가 바뀌면 MSA 위반이며 Gateway가 SPOF화됨. Gateway는 범용 인프라(session, channel 등)만 보유

### 문서/프로세스 규칙
- **백로그**: `docs/BACKLOG.md`에 기록. 별도 백로그 파일 생성 금지. 완료 항목은 즉시 삭제 (git이 이력 보존)
- **문서 경로**: `docs/{project}/plans/`, `progress/`, `review/` — 공통은 `docs/apex_common/`
- **파일명**: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 `date +"%Y%m%d_%H%M%S"` 명령으로 취득한 **정확한 현재 시각** 필수. 추정/반올림 금지
- **review 문서**: 리뷰 항목 상세 포함 필수 — 헤더/통계만 있는 빈 껍데기 금지
- **progress 문서**: 작업 결과 요약 필수
- **TODO/백로그 분리**: review·progress 문서에 TODO·백로그·향후 과제 잔류 금지 — 발견 즉시 `docs/BACKLOG.md`로 이전
- 상세 규칙: `docs/CLAUDE.md` 참조

### 에이전트 작업
- **서브에이전트는 작업 전 관련 CLAUDE.md를 직접 읽는다** — 메인이 발췌할 필요 없이 서브에이전트가 해당 영역의 CLAUDE.md를 직접 Read하여 규칙 파악
- **태스크 완료 후 메인이 auto-review 필요 여부를 자체 판단하여 실행한다** (유저에게 묻지 않음)
- **리뷰 이슈 판단 기준**: "지금 안 고치면 나중에 더 복잡해지는가?" YES면 지금 수정. 스코프 밖이라는 이유로 미루지 않음. 리뷰어는 구현 비용보다 미래 비용을 우선 고려

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

| 영역 | 파일 |
|------|------|
| 빌드/MSVC/아키텍처/벤치마크 | `apex_core/CLAUDE.md` |
| 문서 작성/리뷰/브레인스토밍 | `docs/CLAUDE.md` |
| 도구/플러그인 캐시/auto-review | `apex_tools/CLAUDE.md` |
| CI/CD 트러블슈팅 | `.github/CLAUDE.md` |
