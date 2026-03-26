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
| ⑥ | **문서 갱신** | CLAUDE.md, Apex_Pipeline.md, BACKLOG.json(export), README, progress 등 | 갱신된 문서 |
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

기존 6종 hook이 도구 호출 시 핵심 게이트를 강제한다 (빌드 경로, PR 머지 차단, 핸드오프, 리베이스, 백로그 편집/읽기). 상세: `.claude/settings.json` — 모든 hook은 `apex-agent` Go 바이너리가 처리.

## 모노레포 구조

`apex_core/`(코어), `apex_shared/`(공유 라이브러리+어댑터), `apex_services/`(MSA), `apex_infra/`(Docker/K8s), `apex_tools/`(CLI/스크립트/auto-review), `docs/`(전체 문서 중앙 집중)

## 빌드

- `"<프로젝트루트절대경로>/apex_tools/apex-agent/run-hook" queue build debug` (bash 셸). **반드시 절대 경로 사용** — `pwd`나 git root로 경로를 구한 뒤 조합
- 개별 타겟 빌드: `"<프로젝트루트절대경로>/apex_tools/apex-agent/run-hook" queue build debug --target apex_core`
- `build.bat`, `cmake`, `ninja` 등 직접 호출 시 PreToolUse hook이 차단
- **빌드는 항상 `run_in_background: true`로 실행** — `timeout` 파라미터 절대 설정 금지. 완료 알림까지 무한 대기
- **빌드 결과는 로그 파일로 확인** — 빌드 출력은 `$LOCALAPPDATA/apex-agent/logs/build_*.log`에 기록됨. 완료 후 background task output에서 로그 경로를 확인하고 Read tool로 직접 읽기 (stdout 파이프 미사용)
- **서브에이전트 빌드 금지** — 각 서브에이전트 작업 취합 후 메인이 직접 빌드
- **빌드 오류 책임**: 빌드 실패는 작업 프로세스 내에서 해결. auto-review와 별개 — 작업 완료+빌드 성공 확인 후 리뷰 진입

## clangd

- **clangd가 Claude Code에 LSP로 연결**되어 있음 — 별도 에디터 없음
- `.clangd` 설정이 MSVC 플래그를 clang 호환으로 변환 (Remove + Add)
- `compile_commands.json`은 `build.bat`이 빌드 후 루트로 복사
- vcpkg 인클루드 경로: `-external:I` → Remove로 제거되므로 `.clangd` Add에 `-I`로 재추가 필수

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성. 상위 3자리는 루트 `VERSION` 파일, 4번째 자리는 CI auto-tag가 자동 관리. 상세: `docs/apex_infra/cicd_guide.md` §10
- **현재**: v0.6.4 — CI/CD 고도화 + SocketBase virtual interface(BACKLOG-133) + HttpServerBase/AdminHttpServer 런타임 로그 레벨(BACKLOG-179) + BlockingTaskExecutor CPU offload(BACKLOG-146) + SecureString 메모리 제로화+constant-time 비교+SSO 버퍼 제로화(BACKLOG-135,245,246) + ESO/AWS SM 연동(BACKLOG-198) + ESO per-service RBAC(BACKLOG-252). Gateway RL RedisAdapter 라이프사이클 통합(BACKLOG-250) + rate_limiter_ atomic 제거(BACKLOG-251). full auto-review 22건 수정(SpscQueue 레이스, Redis 콜백, refresh token 트랜잭션, Helm 보안 강화). Helm Rollout CRD 지원, 3단계 검증 파이프라인. K8s Helm logging.file 비활성화(BACKLOG-255). 보안 소탕: Kafka manual commit(BACKLOG-247) + Gateway 시스템 메시지 인증(BACKLOG-249) + auth exempt rate limit(BACKLOG-248) + K8s NetworkPolicy(BACKLOG-253). 대시보드 /branches 외부 세션 감지(Hook+mtime 폴백+좀비 정리), ref count 중복 세션 추적, EXTERNAL/MANAGED Sync 차단(BACKLOG-242). Whisper O(1) core routing — Auth SessionStore core_id 저장 + Gateway 단일 코어 직접 전달(BACKLOG-149)
- **도구**: apex-agent Go 백엔드 — HTTP 대시보드(`localhost:7600`, 5개 페이지: Dashboard/Backlog/Handoff/Queue/Branches), 빌드/머지 큐, 백로그 DB+CLI, 핸드오프 상태 머신, cleanup, 일별 로그 분할, 워크스페이스 모듈(디렉토리 스캔/동기화+REST API), 백로그 blocked_reason(⚠ 뱃지+CLI show 출력), session CLI(run/start/stop/status/send), 세션 서버(:7601, ConPTY+WebSocket+Watchdog), 리버스 프록시(:7600→:7601), 외부 세션 감지(Hook+mtime 폴백+좀비 정리), ref count 중복 세션 추적, EXTERNAL/MANAGED Sync 차단, queue build 콘솔 실시간 출력. 주요 완료: PR #126(재작성), #130(대시보드), #145(큐 히스토리), #150(IPC), #156(stale guard), #162(RESOLVED 원복 방지), #164(로직 강화), #167(로그 분할), #169(idle timeout 제거+auto-restart), #174(notify merge 통합), #176(Go build.bat 허용+watchdog), #195(워크스페이스+blocked_reason Phase 1), #199(세션 서버 Phase 2-3), #200(BranchInfo JOIN+graceful shutdown), #206(/branches 외부 세션 감지+검증)
- **테스트**: CORE+SHARED 유닛 테스트 커버리지 보강 완료 (PR #146) — 핵심 경로 22건 추가. CrashHandler death test + Gateway config parser 22케이스 + intrusive_ptr 벤치마크 (PR #189, BACKLOG-142,171,200)
- **다음**: v1.0.0.0 (프레임워크 완성)
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
  1. (사전) `apex-agent queue build debug` (빌드 + 테스트 재검증) — **빌드 스킵 조건** (A 또는 B 충족 시):
     - **A)** 문서/주석 전용 PR + rebase로 받은 변경도 문서뿐 + 기존 코드 빌드+테스트 통과
     - **B)** rebase 충돌 없음 + 추가 코드 변경 없음 + 최신 PR CI 통과 (3개 모두)
  2. `apex-agent handoff notify merge --summary "..."` — **머지의 유일한 진입점**. 데몬이 내부에서 lock 획득→backlog export+commit→rebase→push→gh pr merge→핸드오프 정리→checkout main을 원자적으로 수행
- **`gh pr merge` 직접 호출 금지** — validate-merge hook이 무조건 차단. `notify merge`만 사용
- **`gh pr create`에 `--base main` 강제** — `--base`가 main이 아닌 값이면 hook 차단 (stacked PR 방지). `--base` 미지정 시 GitHub 기본값(main) 사용되므로 허용
- **머지 전 필수 갱신**: `docs/Apex_Pipeline.md`, `CLAUDE.md` 로드맵, `README.md`, progress 문서(`docs/{project}/progress/`), `docs/apex_core/apex_core_guide.md`(코어 영역 변경 시), `docs/apex_core/log_patterns_guide.md`(로깅 영역 변경 시), `docs/apex_infra/cicd_guide.md`(CI/CD 워크플로우 변경 시) — 머지 직전에 갱신하므로 **완료 상태로 기재** (구현 중/리뷰 중이 아님). `docs/BACKLOG.json`(`backlog export`)은 `notify merge`가 자동 수행
- **브랜치 이관 금지**: 작업 시작 브랜치 = PR 브랜치. 중간에 새 브랜치로 이관하지 않음. 불가피하면 새 브랜치 푸시 시점에 `git push origin --delete {원본브랜치}`로 원본 리모트 즉시 삭제 — cleanup 스크립트가 탐지 불가한 고아 브랜치 방지
- **작업 완료 후 브랜치 정리**: 모든 작업이 완전히 끝나면 `apex-agent cleanup --execute` 실행 — 머지 완료 브랜치 + 잔여 리모트 브랜치 + 워크스페이스 복사본 로컬 브랜치 일괄 정리. 플래그 없이 실행하면 dry-run (삭제 없이 대상만 표시). `--dry-run` 플래그는 없음

### 브랜치 인수인계 (Branch Handoff)
- **도구**: `apex-agent handoff`. 상세: `apex_tools/apex-agent/CLAUDE.md` § 핸드오프 CLI
- **착수 전**: `handoff backlog-check <N>` 으로 중복 착수 확인
- **브랜치 생성** (`git checkout -b` 직접 호출 금지 — hook 차단):
  - 백로그 연결: `notify start --branch-name feature/foo --backlog N[,M,...] --summary "..." --scopes core,shared`
  - 비백로그: `notify start job --branch-name feature/foo --summary "..." --scopes tools`
  - `--summary`, `--scopes` 필수. `--skip-design`: 설계 불필요 시 바로 IMPLEMENTING 진입
- **상태 전이**: `notify start` → `notify design` → `notify plan` → `notify merge`. 중도 포기: `notify drop --reason "..."`
- **Hook 강제** — 핸드오프 미등록 시 Edit/Write/commit 차단, 설계 단계에서 소스 파일 편집 차단, `git checkout -b` 등 직접 브랜치 생성 차단. 상세: `apex_tools/apex-agent/CLAUDE.md`

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

### CI/CD 가이드 유지보수
- **갱신 트리거**: CI/CD 워크플로우 변경 시 `docs/apex_infra/cicd_guide.md` 동시 갱신 필수
  - ci.yml 잡 추가/삭제/조건 변경
  - docker-bake.hcl 변수/타겟 변경
  - service.Dockerfile 빌드 단계 변경
  - Helm 차트 배포 전략 변경 (Rollout canary steps 등)
  - 새 검증 단계 추가 (스모크 테스트 시나리오 등)
  - Docker 이미지 태깅 전략 변경
- **머지 전 체크**: 코어 영역 PR에서 가이드 갱신 여부 확인

### apex-agent 워크플로우 가이드 유지보수
- **갱신 트리거**: apex-agent 기능 변경 시 `docs/apex_tools/apex_agent_workflow_guide.md` + `apex_tools/apex-agent/CLAUDE.md` 동시 갱신 필수
  - 모듈 추가/삭제 (workspace, session 등)
  - 핸드오프 상태 머신 변경
  - 백로그 상태 전이 변경 (blocked_reason 등 신규 필드)
  - Hook 게이트 추가/삭제/조건 변경
  - IPC 액션 추가/삭제
  - config.toml 섹션 추가
  - 머지 파이프라인 단계 변경
- **정합성**: `apex_tools/apex-agent/CLAUDE.md`(CLI 레퍼런스)와 `docs/apex_tools/apex_agent_workflow_guide.md`(워크플로우 운영 레퍼런스)는 동일 사실을 다른 관점에서 기술 — 양쪽 불일치 금지

### 백로그
- **저장**: DB가 source of truth, `docs/BACKLOG.json`은 git 백업. 상세 운영 규칙: `docs/CLAUDE.md` § 백로그 운영
- **파일 직접 접근 금지** — `validate-backlog` hook이 Read/Edit/Write 모두 차단. CLI(`backlog list/show/add/update/resolve/release/fix/export`)만 사용
- **미래 작업은 백로그 등록 필수** — 설계 문서의 "향후 확장"에만 남기는 것은 불충분. 백로그가 작업 발견의 단일 진입점
- **라이프사이클 워크플로우**:

  | 커맨드 | 시점 | 동작 | 책임 |
  |--------|------|------|------|
  | `backlog add --fix` | 이슈 발견 즉시 수정 | OPEN→FIXING + junction 연결 | 등록자 |
  | `backlog add --no-fix` | 이슈 발견, 지금 안 고침 | OPEN 유지 (junction 없음) | 등록자 |
  | `backlog fix N` | 기존 OPEN 백로그 착수 | OPEN→FIXING + junction 연결 | 착수자 |
  | `backlog resolve N` | 수정 완료 | →RESOLVED | **수정한 사람** |
  | `backlog release N` | FIXING 포기 | FIXING→OPEN (junction 해제) | 포기자 |
  | `backlog export` | 머지 직전 (⑥단계) | DB→JSON 백업 | 머지 수행자 |

  - 활성 브랜치에서 `backlog add` 시 `--fix`/`--no-fix` 필수 (미지정 시 에러)

  **핵심 원칙 — "고친 사람이 resolve한다":**
  - 코드를 수정하여 백로그 이슈를 해결했다면, **반드시 `backlog resolve N --resolution FIXED` 호출**
  - `--no-fix`로 등록한 건도 동일 — 같은 PR에서 우연히 고쳐졌어도 resolve 필수
  - 커밋 메시지에 `BACKLOG-N`을 적었다면 해당 백로그의 resolve 여부를 반드시 확인
  - **머지 전 체크**: `backlog list --status OPEN`으로 현재 작업에서 해결된 항목이 없는지 확인

### 문서
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
  3. 나머지 → `backlog add`로 등급·시간축 분류 완료하여 등록
- **버그/장애 추적 시 로그 패턴 가이드 필독** — 로그 분석이 수반되는 디버깅 작업에서는 `docs/apex_core/log_patterns_guide.md`를 사전 참조. 정상/비정상 패턴 판별 기준과 컴포넌트별 확인 포인트가 정리되어 있음

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

| 영역 | 파일 |
|------|------|
| 빌드/MSVC/아키텍처/벤치마크 | `apex_core/CLAUDE.md` |
| 문서 작성/리뷰/브레인스토밍 | `docs/CLAUDE.md` |
| 도구/플러그인 캐시/auto-review | `apex_tools/CLAUDE.md` |
| **apex-agent Go 백엔드** ★ | `apex_tools/apex-agent/CLAUDE.md` |
| **apex-agent 워크플로우 가이드** (상태 전이, 동시성, 데이터 정합성) | `docs/apex_tools/apex_agent_workflow_guide.md` |
| E2E 테스트 실행/트러블슈팅 | `apex_services/tests/e2e/CLAUDE.md` |
| CI/CD 트러블슈팅 | `.github/CLAUDE.md` |
| **CI/CD 파이프라인 가이드** (전체 구조, 설정 변경, 트러블슈팅) | `docs/apex_infra/cicd_guide.md` |
| 프레임워크 가이드 (서비스 개발 API + 내부 아키텍처) | `docs/apex_core/apex_core_guide.md` |
| 로그 패턴 가이드 (정상/비정상 패턴, 트러블슈팅) | `docs/apex_core/log_patterns_guide.md` |

**★ `apex_tools/apex-agent/CLAUDE.md` 필독** — hook 게이트, 백로그 Import/Export, 핸드오프, 큐 잠금, 머지 전 워크플로우 등 모든 에이전트 자동화의 핵심 규칙이 여기에 있음. 백로그 상태 관리(Source of Truth 분리), 바이너리 설치 절차, validate-build 허용 목록 등 실수하기 쉬운 항목 포함.
