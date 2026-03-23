# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 워크플로우

모든 작업은 아래 7단계 중 해당하는 단계를 순서대로 밟는다. 착수 시 스킵 조건을 평가하여 체크리스트(TaskCreate)를 생성한다.

| # | 단계 | 핵심 행위 | 산출물 |
|---|------|-----------|--------|
| ① | **착수** | **브랜치 생성 최우선**, 백로그 확인, 핸드오프 backlog-check | 브랜치 |
| ② | **설계** | 브레인스토밍 → 스펙 문서 | `docs/{project}/plans/` |
| ③ | **구현** | 코드 작성, clang-format | 소스 변경 |
| ④ | **검증** | 로컬 빌드+테스트 (`apex-agent queue build`) → PR → CI 검증, 실패 시 재수정. rebase는 `enforce-rebase` hook이 push/PR 시 자동 강제. **CI 재대기 불필요 조건**: 이전 CI에서 코드 변경이 통과했고 추가 push가 문서/지침만이면 재대기 스킵 (auto-review에서 코드를 수정했다면 코드 변경이므로 CI 재대기 필수). **CI 폴링**: `gh run watch` 사용 시 `--interval 30` 필수 (기본 3초 폴링은 API rate limit 소진). **④⑤ 병렬화**: CI 폴링 시작 직후 auto-review를 병렬로 디스패치 — CI 대기 시간에 리뷰를 동시 진행. CI 실패와 리뷰 이슈를 합산하여 한 번에 수정 | 빌드+CI 성공 |
| ⑤ | **리뷰** | auto-review 실행, 이슈 수정 | `docs/{project}/review/` |
| ⑥ | **문서 갱신** | CLAUDE.md, Apex_Pipeline.md, BACKLOG.md, README, progress 등 | 갱신된 문서 |
| ⑦ | **머지** | 상세: § Git/브랜치 머지 참조 | main에 머지 |

**① 착수 필수 순서**: 브랜치 생성 → 핸드오프 등록 → 이후 모든 작업. 상태 확인·분석·논의 등 어떤 작업이든 브랜치가 먼저 존재해야 한다. 브랜치 없이 코드 탐색이나 설계 논의를 시작하지 않는다.

**스킵 조건:**

| 단계 | 스킵 가능 조건 |
|------|---------------|
| ② 설계 | 변경이 단순하고 설계 판단이 불필요 (오타 수정, 1파일 이하 기계적 변경, 문서 전용) |
| ③ 구현 | 문서 전용 작업 |
| ④ 검증 | 문서 전용 작업 (코드 변경 없음), 또는 소스 파일 변경이 주석만인 경우 (빌드·CI 대기 불필요) |
| ⑤ 리뷰 | 문서 전용 작업, 또는 변경 범위가 극히 작아 리뷰 ROI가 없는 경우 |
| ①⑥⑦ | **스킵 불가** — 모든 작업에 필수 |

기존 hook 5개가 도구 호출 시 핵심 게이트를 강제한다 (빌드 경로, 머지 lock, 핸드오프, 리베이스). 상세: `.claude/settings.json` — 모든 hook은 `apex-agent` Go 바이너리가 처리.

## 모노레포 구조

`apex_core/`(코어), `apex_shared/`(공유 라이브러리+어댑터), `apex_services/`(MSA), `apex_infra/`(Docker/K8s), `apex_tools/`(CLI/스크립트/auto-review), `docs/`(전체 문서 중앙 집중)

## 빌드

- `"<프로젝트루트절대경로>/apex_tools/apex-agent/run-hook" queue build debug` (bash 셸). **반드시 절대 경로 사용** — `pwd`나 git root로 경로를 구한 뒤 조합
- 개별 타겟 빌드: `"<프로젝트루트절대경로>/apex_tools/apex-agent/run-hook" queue build debug --target apex_core`
- `build.bat`, `cmake`, `ninja` 등 직접 호출 시 PreToolUse hook이 차단
- **빌드는 항상 `run_in_background: true`로 실행** — `timeout` 파라미터 절대 설정 금지. 완료 알림까지 무한 대기
- **서브에이전트 빌드 금지** — 각 서브에이전트 작업 취합 후 메인이 직접 빌드
- **빌드 오류 책임**: 빌드 실패는 작업 프로세스 내에서 해결. auto-review와 별개 — 작업 완료+빌드 성공 확인 후 리뷰 진입

## clangd

- **clangd가 Claude Code에 LSP로 연결**되어 있음 — 별도 에디터 없음
- `.clangd` 설정이 MSVC 플래그를 clang 호환으로 변환 (Remove + Add)
- `compile_commands.json`은 `build.bat`이 빌드 후 루트로 복사
- vcpkg 인클루드 경로: `-external:I` → Remove로 제거되므로 `.clangd` Add에 `-I`로 재추가 필수

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성
- **현재**: v0.5.10.7 — ASAN UAF 수정 (Session core_executor_ 추가, timer/write_pump executor 분리)
- **도구**: apex-agent Go 백엔드 완전 재작성 완료 (#126) — bash 11종 → Go 단일 바이너리 (데몬+SQLite+IPC, 14K LOC). cleanup 핫픽스 + 핸드오프 구조 강화 완료. workflow 공유 레이어 완료 — CLI 인라인 로직을 `internal/workflow/` 패키지로 추출 (IPCFunc 추상화, Start/Merge/Drop 파이프라인), 백로그 import/export 자동 동기화, BACKLOG-157 rebase abort 에러 핸들링
- **다음**: v0.6 (운영 인프라) → v1.0.0.0 (프레임워크 완성)
- 상세: `docs/Apex_Pipeline.md` §10

## 전역 규칙

### Git / 브랜치
- **커밋 메시지는 한국어로 작성** — `feat(core): 코루틴 할당기 추가` 형태 (타입+스코프 영어, 설명 한국어)
- **커밋 메시지에서 백로그 번호는 `BACKLOG-N` 형식** — `#N` 금지 (GitHub PR/Issue 자동 링크 충돌)
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **새 브랜치 생성 전 최신화 필수**: `git fetch origin main && git pull origin main` 실행 후 최신 main 기반으로 브랜치 생성
- **커밋 즉시 리모트 푸시** — 모든 커밋 후 `git push` 실행
- **머지**: 리뷰 이슈 0건 → 아래 순서로 실행:
  1. `apex-agent queue merge acquire` (lock 획득까지 대기)
  2. rebase는 `enforce-rebase` hook이 push 시 자동 실행 (충돌 시 차단+안내, 에이전트가 resolve 후 재시도)
  3. `apex-agent queue build debug` (빌드 + 테스트 재검증) — **빌드 스킵 조건** (A 또는 B 충족 시 스킵): **A)** ①+② 동시 충족: ① 문서 전용 PR (`.md`, `.txt`, `docs/` 변경만) 또는 소스 파일 변경이 주석만인 PR ② rebase로 받은 변경이 문서뿐이고 PR 자체의 코드 빌드+테스트가 이미 통과된 경우 (주석만 변경 시 빌드+CI 대기 자체가 불필요) **B)** 단독 충족: rebase 시 충돌 없음 + rebase 이후 추가 코드 변경 없음 + 최신 PR CI가 통과 확인됨 (3개 조건 모두)
  4. `git push --force-with-lease`
  5. `gh pr merge --squash --admin`
  6. `apex-agent queue merge release`
- **머지 lock 없이 `gh pr merge` 실행 금지** (PreToolUse hook이 차단)
- **머지 전 필수 갱신**: `docs/Apex_Pipeline.md`, `CLAUDE.md` 로드맵, `README.md`, `docs/BACKLOG.md`, progress 문서(`docs/{project}/progress/`), `docs/apex_core/apex_core_guide.md`(코어 영역 변경 시) — 머지 직전에 갱신하므로 **완료 상태로 기재** (구현 중/리뷰 중이 아님)
- **브랜치 이관 금지**: 작업 시작 브랜치 = PR 브랜치. 중간에 새 브랜치로 이관하지 않음. 불가피하면 새 브랜치 푸시 시점에 `git push origin --delete {원본브랜치}`로 원본 리모트 즉시 삭제 — cleanup 스크립트가 탐지 불가한 고아 브랜치 방지
- **작업 완료 후 브랜치 정리**: 모든 작업이 완전히 끝나면 `apex-agent cleanup --execute` 실행 — 머지 완료 브랜치 + 잔여 리모트 브랜치 일괄 정리. 플래그 없이 실행하면 dry-run (삭제 없이 대상만 표시). `--dry-run` 플래그는 없음

### 브랜치 인수인계 (Branch Handoff)
- **도구**: `apex-agent handoff` (Go 바이너리). 상세: `apex_tools/apex-agent/CLAUDE.md` § 핸드오프 CLI
- **착수 전**: `apex-agent handoff backlog-check <N>` 으로 중복 착수 확인 (백로그 항목이 있는 경우)
- **브랜치 생성** (`git checkout -b` 직접 호출 금지 — hook 차단):
  - 백로그 연결: `notify start --branch-name feature/foo --backlog N --summary "..." --scopes core,shared`
  - 비백로그: `notify start job --branch-name feature/foo --summary "..." --scopes tools`
  - `--summary`, `--scopes` 필수. 핸드오프 등록과 git 브랜치 생성이 원자적으로 수행
- **상태 머신**: `notify start` → `started` → `notify design` → `design-notified` → `notify plan` → `implementing` → `notify merge` (active에서 삭제, history로 이관)
  - `notify start --skip-design`: 설계 불필요 시 바로 `implementing`으로 진입
  - `notify drop --reason "..."`: 작업 중도 포기 (active에서 삭제, history에 DROPPED 기록)
  - 각 단계 전환은 선행 상태 검증 (잘못된 전환 자동 차단)
- **DB 구조**: `active_branches` (활성 작업), `branch_history` (머지/포기 이력). 머지/포기 시 active에서 삭제 → history로 이관
- **강제 메커니즘** (Hook 기반):
  - **active 미등록** → 모든 Edit/Write/git commit **차단** (예외 없음)
  - **status=started/design-notified** → 소스 파일(.cpp/.hpp/.h) 편집 **차단**, 비소스(문서 등) 허용
  - **status=implementing** → 모든 파일 허용
  - **main/master 브랜치** → 핸드오프 체크 스킵
  - **git checkout -b / git branch <name>** → validate-build hook이 차단 (notify start 경유 강제)
  - **cleanup** → `active_branches` 조회하여 활성 브랜치 보호 + CWD 보호
- **알림**: 설계 확정 시 `notify design` (방향 변경 시 재발행), 머지 완료 시 `notify merge`
- **알림 감지**: PreToolUse probe가 Edit/Write/Bash 호출 시 자동 감지 (수동 `check` 불필요). 미ack 알림은 경고 표시, 머지 시점은 hook이 자동 차단
- **ack**: 알림 수신 시 `apex-agent handoff ack --id <N> --action <no-impact|will-rebase|rebased|design-adjusted|deferred>`

### 설계 원칙
- **코어 프레임워크 가이드 필독**: 코어 영역 또는 서비스 코드 작성·변경 시 `docs/apex_core/apex_core_guide.md`를 반드시 사전 참조. shared-nothing, per-core 독립, intrusive_ptr 수명 관리 등 프레임워크 설계 원칙을 위배하는 코드 금지
- **Gateway 서비스 독립성**: Gateway는 개별 서비스의 도메인 지식에 절대 의존 금지. 서비스 추가/변경 시 Gateway 코드가 바뀌면 MSA 위반이며 Gateway가 SPOF화됨. Gateway는 범용 인프라(session, channel 등)만 보유

### 저작권 헤더
- **새 소스/스크립트 파일 생성 시 MIT 저작권 헤더 필수** — `// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.` (스크립트는 shebang 다음 줄 `#` 주석)

### 포맷팅
- **빌드 전 clang-format 필수** — 코드 변경 후 빌드 전에 `find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i` 실행. CI `format-check`에서 불일치 시 빌드 실패

### 경고 정책 (Zero Warning)
- **MSVC, GCC, Clang 3개 컴파일러 모두 경고 0건 필수** — `-Werror`/`/WX`가 CI에서 강제되므로 경고가 곧 빌드 실패
- **새 코드 작성 시 경고 발생 금지** — 로컬 빌드(`/W4 /WX`)에서 먼저 확인, CI(GCC `-Wall -Wextra -Wpedantic -Werror`)에서 최종 검증
- **경고 억제는 최후의 수단** — 코드 수정으로 해결 우선. 불가피한 경우(의도적 `alignas` 패딩, 외부 라이브러리 코드 등)만 `cmake/ApexWarnings.cmake`에 명시적 비활성화 + 사유 주석
- **새 타겟 추가 시 `apex_set_warnings()` 필수** — `cmake/ApexWarnings.cmake`에 정의된 공통 경고 함수를 모든 타겟에 적용. 외부 C 라이브러리(`apex_bcrypt` 등), INTERFACE 라이브러리 제외

### 프레임워크 가이드 유지보수
- **갱신 트리거**: 코어 인터페이스 변경 시 `docs/apex_core/apex_core_guide.md` 동시 갱신 필수
  - ServiceBase 훅 추가/삭제/시그니처 변경
  - 핸들러 등록 API 변경 (handle, route, kafka_route, set_default_handler)
  - ConfigureContext / WireContext 필드 변경
  - ServerConfig 필드 추가/삭제
  - 새 Phase 도입 또는 Phase 순서 변경
  - 새 어댑터 타입 추가
- **갱신 범위**: 레이어 1(API)은 직접 수정, 레이어 2(내부)는 ADR 포인터 정합성 확인
- **머지 전 체크**: 코어 영역 PR에서 가이드 갱신 여부 확인

### 문서/프로세스 규칙
- **백로그**: `docs/BACKLOG.md`에 기록. 별도 백로그 파일 생성 금지. 운영 규칙: `docs/CLAUDE.md` § 백로그 운영
- **백로그 파일 직접 편집 금지** — `docs/BACKLOG.md`, `docs/BACKLOG_HISTORY.md`는 `validate-backlog` hook이 차단. `backlog add/update/resolve/release/export` CLI 사용 필수
- **미래 작업은 백로그 등록 필수** — 설계 문서의 "향후 확장" 섹션에만 남기는 것은 불충분. 백로그가 작업 발견의 단일 진입점
- **파일명**: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 `date +"%Y%m%d_%H%M%S"` 명령으로 취득한 **정확한 현재 시각** 필수. 추정/반올림 금지
- 문서 경로, 작성 규칙, 리뷰 규칙 상세: `docs/CLAUDE.md` 참조

### 에이전트 작업
- **서브에이전트는 작업 전 관련 CLAUDE.md를 직접 읽는다** — 메인이 발췌할 필요 없이 서브에이전트가 해당 영역의 CLAUDE.md를 직접 Read하여 규칙 파악
- **태스크 완료 후 메인이 auto-review 필요 여부를 자체 판단하여 실행한다** (유저에게 묻지 않음)
- **구현 계획 실행 방식은 자체 판단** — subagent-driven vs inline 선택을 유저에게 묻지 않고 작업 특성(순차 의존 vs 독립 병렬)에 따라 직접 결정. 선택한 방식과 이유를 한 줄로 알리고 바로 진행
- **리뷰 이슈 판단 기준**: "지금 안 고치면 나중에 더 복잡해지는가?" YES면 지금 수정. 스코프 밖이라는 이유로 미루지 않음. 리뷰어는 구현 비용보다 미래 비용을 우선 고려
- **작업 간 발견 이슈 처리** (유저에게 묻지 않고 자체 판단):
  1. 자기 작업으로 인한 이슈 → **즉시 수정** (컨텍스트가 살아있을 때 비용 최저)
  2. 기존·스코프 외라도 수정 비용 낮거나 ROI 높은 이슈 → **즉시 수정**
  3. 나머지 → `docs/BACKLOG.md`에 등급·시간축 분류 완료하여 등록

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

| 영역 | 파일 |
|------|------|
| 빌드/MSVC/아키텍처/벤치마크 | `apex_core/CLAUDE.md` |
| 문서 작성/리뷰/브레인스토밍 | `docs/CLAUDE.md` |
| 도구/플러그인 캐시/auto-review | `apex_tools/CLAUDE.md` |
| **apex-agent Go 백엔드** ★ | `apex_tools/apex-agent/CLAUDE.md` |
| E2E 테스트 실행/트러블슈팅 | `apex_services/tests/e2e/CLAUDE.md` |
| CI/CD 트러블슈팅 | `.github/CLAUDE.md` |
| 프레임워크 가이드 (서비스 개발 API + 내부 아키텍처) | `docs/apex_core/apex_core_guide.md` |

**★ `apex_tools/apex-agent/CLAUDE.md` 필독** — hook 게이트, 백로그 Import/Export, 핸드오프, 큐 잠금, 머지 전 워크플로우 등 모든 에이전트 자동화의 핵심 규칙이 여기에 있음. 백로그 상태 관리(Source of Truth 분리), 바이너리 설치 절차, validate-build 허용 목록 등 실수하기 쉬운 항목 포함.
