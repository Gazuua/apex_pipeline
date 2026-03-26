# apex-agent 워크플로우 가이드

apex-agent가 7단계 워크플로우에서 어떻게 개입하는지를 정리한 운영 레퍼런스.
CLI 레퍼런스는 `apex_tools/apex-agent/CLAUDE.md` 참조.

## 워크플로우 전체 흐름

```
① 착수 ──→ ② 설계 ──→ ③ 구현 ──→ ④ 검증 ──→ ⑤ 리뷰 ──→ ⑥ 문서 ──→ ⑦ 머지
   │           │           │           │           │           │          │
   ▼           ▼           ▼           ▼           ▼           ▼          ▼
StartPipeline  notify     notify      queue      auto-      backlog   MergePipeline
 ├─validate    design     plan        build      review     export    ├─rebase
 ├─DB TX                  backlog     ├─lock                          ├─export
 └─git branch             add/fix     ├─build.bat                    ├─notify merge
                           blocked    └─unlock                       └─cleanup

횡단 기능:
  workspace scan ── 데몬 시작 시 멀티 브랜치 디렉토리 자동 탐색 → local_branches DB 등록
  workspace sync ── main 브랜치 일괄 동기화 (git fetch + pull)
```

## 단계별 상세

### ① 착수 — StartPipeline

```
handoff notify start --backlog N --branch-name feature/foo --summary "..." --scopes core

Phase 1: ValidateNewBranch ── git local+remote 중복 확인 (TX 외부)
Phase 2: IPC notify-start ── 단일 DB TX
         ├─ 기존 branch 있으면 stale 정리 (FIXING→OPEN 복귀)
         ├─ active_branches INSERT
         ├─ branch_backlogs INSERT (N건)
         └─ backlog OPEN→FIXING (SetStatusWith 가드)
Phase 3: CreateAndPushBranch ── git checkout -b + push (TX 외부)
```

DB 테이블 변경: `active_branches`(INSERT), `branch_backlogs`(INSERT), `backlog_items`(FIXING 전이)

### ② 설계

```
handoff notify design --summary "..."

IPC: handoff/notify-transition (type="design")
상태: STARTED → DESIGN_NOTIFIED (TX 내 read-validate-update)
```

Hook: `handoff-probe`가 STARTED/DESIGN_NOTIFIED에서 소스 파일 편집 차단

### ③ 구현

```
handoff notify plan --summary "..."

IPC: handoff/notify-transition (type="plan")
상태: DESIGN_NOTIFIED → IMPLEMENTING (TX 내)
```

IMPLEMENTING 상태에서 모든 파일 편집 허용. `backlog add --fix`로 추가 백로그 연결 가능.

작업 중 유저 결정이 필요하면:
```
apex-agent backlog update N --blocked "사유: A vs B 중 택1"   # blocked_reason 설정
→ 대시보드 ⚠ 뱃지 표시 (Phase 3), 에이전트는 다른 작업 진행 가능
→ 유저 결정 후 에이전트가:
apex-agent backlog update N --blocked ""                       # blocked_reason 해제
```
status는 FIXING 유지, 별도 상태 전이 없음.

### ④ 검증 — Queue Lock

```
apex-agent queue build debug

FIFO Lock: INSERT WAITING → 폴링 → tryPromote(CAS TX) → ACTIVE → build.bat → DONE
```

동시 빌드 시 낮은 ID가 먼저 promote (FIFO 보장). build/merge 채널 독립.

### ⑤ 리뷰

auto-review 스킬 실행. 발견 이슈를 `backlog add/resolve/release`로 처리.

### ⑥ 문서 갱신

`backlog export`로 DB→JSON 동기화. CLAUDE.md, Apex_Pipeline.md, README.md 갱신.

### ⑦ 머지 — MergePipeline

```
1. queue merge acquire ── FIFO 블로킹
2. git push --force-with-lease ── enforce-rebase hook 자동 실행
3. gh pr merge --squash --delete-branch --admin
   ├─ validate-merge: lock 소유자 확인 + --delete-branch 강제
   └─ validate-handoff: FIXING 백로그 0건 확인
4. queue merge release
5. handoff notify merge --summary "..."
   ├─ RebaseOnMain
   ├─ SyncExport + autoCommitExport
   ├─ CheckoutMain
   └─ IPC notify-merge (TX: FIXING 재확인 + active→history 이관)
```

## Handoff 상태 전이

```
                notify design          notify plan
     STARTED ──────────→ DESIGN_NOTIFIED ──────────→ IMPLEMENTING
        │                                                  │
        └── --skip-design ────────────────────────────────┘

     IMPLEMENTING ──→ notify merge ──→ (MERGED, history 이관)
     모든 active ───→ notify drop ──→ (DROPPED, FIXING→OPEN 복귀)
```

| 현재 상태 | design | plan | merge | drop |
|-----------|--------|------|-------|------|
| STARTED | DESIGN_NOTIFIED | 에러 | 에러 | DROPPED |
| DESIGN_NOTIFIED | 에러 | IMPLEMENTING | 에러 | DROPPED |
| IMPLEMENTING | 에러 | 에러 | MERGED | DROPPED |

## Backlog 상태 전이

```
           add --fix / notify start    resolve
    OPEN ─────────────────────→ FIXING ─────────→ RESOLVED
     ▲                            │ ▲
     │       release / drop       │ │
     └────────────────────────────┘ │
                                    │
                              blocked_reason
                              (상태 불변, 표시용)
                              설정: --blocked "사유"
                              해제: --blocked ""

    RESOLVED → OPEN: DB 레벨 차단 (SetStatusWith 가드)
```

| 현재 | fix | resolve | release | SetStatus(OPEN) | import | blocked |
|------|-----|---------|---------|-----------------|--------|---------|
| OPEN | FIXING | RESOLVED | 에러 | 에러 | 불변 | N/A |
| FIXING | 에러 | RESOLVED | OPEN | OPEN | 불변 | 설정/해제 가능 |
| RESOLVED | 에러 | 에러 | 에러 | 에러 (가드) | 불변 | N/A |

- `blocked_reason`은 FIXING 상태에서만 의미 있음 — status 전이와 무관한 표시용 필드
- `DashboardBlockedCount()`: `status='FIXING' AND blocked_reason IS NOT NULL AND blocked_reason != ''`

## Hook 게이트 매트릭스

| Hook | 트리거 | 데몬 의존 | 데몬 down 시 |
|------|--------|-----------|-------------|
| validate-build | Bash (cmake/ninja/build.bat/git branch create) | 불필요 | 정상 동작 |
| validate-backlog | Edit/Write/Read (BACKLOG.json) | 불필요 | 정상 동작 |
| enforce-rebase | Bash (git push/gh pr create) | 불필요 | 정상 동작 |
| validate-merge | Bash (gh pr merge/create) | 필요 | **차단 (fail-close)** |
| validate-handoff | Bash (git commit/gh pr merge) | 필요 | **차단 (fail-close)** |
| handoff-probe | Edit/Write (파일 편집) | 필요 | **차단 (fail-close)** |

### handoff-probe 편집 게이트

| 상태 | 소스 파일 (.cpp/.hpp/.go 등) | 비소스 파일 (.md 등) |
|------|---------------------------|---------------------|
| STARTED | 차단 | 허용 |
| DESIGN_NOTIFIED | 차단 | 허용 |
| IMPLEMENTING | 허용 | 허용 |
| 미등록 | 차단 | 차단 |

## 동시성 보장

### 동시 notify start (같은 백로그)
- SetStatusWith의 `WHERE status != 'FIXING'` DB 가드가 최종 방어
- 먼저 도착한 TX만 성공, 나중 TX는 "already FIXING" 에러

### 동시 build/merge lock
- FIFO 큐 (WAITING INSERT → tryPromote CAS TX)
- 동시에 ACTIVE 불가능 (TX CAS 패턴)

### 데몬 크래시 복구
- queue 잔류 ACTIVE: 다음 Acquire 시 CleanupStale이 PID 확인 후 자동 제거
- active_branches: 데몬 무관 (SQLite 파일 기반)
- 데몬 재시작: `apex-agent daemon start`

## 데이터 정합성

### TX 보호 연산
- NotifyStart: active_branches + branch_backlogs + backlog FIXING (단일 TX)
- NotifyTransition: 상태 읽기 + 검증 + UPDATE (단일 TX)
- finalizeBranch: FIXING 체크 + history 이관 + 삭제 (단일 TX)
- queue Acquire/Release/tryPromote: 각각 단일 TX
- backlog Fix/Release: 상태 변경 + junction 조작 (단일 TX)
- CleanupStale: PID 확인 + DELETE (단일 TX)

### DB-git 불일치 윈도우
- StartPipeline Phase 2(DB) 성공 → Phase 3(git) 실패: DB 잔류. 동일 커맨드 재시도로 복구 (NotifyStart 재등록 로직).
- MergePipeline 중간 실패: notify merge 재시도 가능.
- gh pr merge 성공 → queue release 실패: 재실행 (idempotent) 또는 CleanupStale 자동 처리.

## 워크스페이스 관리 모듈

멀티 브랜치 워크스페이스 디렉토리를 DB에 등록하고 관리하는 횡단 기능. 워크플로우 단계와 독립적으로 동작.

### 개요

```
[workspace].root (예: D:/.workspace)
    ├── apex_pipeline_branch_agent/   → workspace_id: branch_agent
    ├── apex_pipeline_branch_02/      → workspace_id: branch_02
    ├── apex_pipeline_branch_03/      → workspace_id: branch_03
    └── other_project/                → 무시 (접두어 불일치)
```

데몬 시작 시 `scan_on_start=true`면 자동 스캔. 이후 IPC `workspace.scan`으로 수동 트리거.

### DB: `local_branches` 테이블

| 컬럼 | 타입 | 설명 |
|------|------|------|
| `workspace_id` | TEXT PK | 디렉토리명에서 추출 (`branch_02` 등) |
| `directory` | TEXT UNIQUE | 절대 경로 |
| `git_branch` | TEXT | 현재 체크아웃 브랜치 |
| `git_status` | TEXT | CLEAN / DIRTY / UNKNOWN |
| `session_id` | TEXT | Claude Code 세션 ID (Phase 2) |
| `session_pid` | INTEGER | 세션 PID, 0=없음 (Phase 2) |
| `session_status` | TEXT | STOP / MANAGED / EXTERNAL (Phase 2) |
| `session_log` | TEXT | 세션 로그 경로 (Phase 2) |
| `last_scanned` | TEXT | 마지막 스캔 시각 |

### `active_branches`와의 관계

```
local_branches (물리: 디스크에 존재하는 모든 워크스페이스)
     │
     └─ LEFT JOIN active_branches ON workspace_id = branch
        (논리: 핸드오프 등록된 작업 중인 브랜치만)
```

- `local_branches`: 물리 환경 관리 (디스크 존재, git 상태, 세션)
- `active_branches`: 논리 상태 관리 (핸드오프 상태 머신)
- 핸드오프 미등록 브랜치 (main 등)도 `local_branches`에서 관리 가능

### 스캔 동작

1. `[workspace].root`에서 `[workspace].repo_name` 접두어 디렉토리 탐색
2. `.git` 존재 확인 → `git branch --show-current` + `git status --porcelain`
3. `local_branches`에 UPSERT (신규 등록 / 기존 갱신)
4. DB에 있지만 디스크에 없는 항목 자동 제거 (cleanup)

### 동기화

`workspace sync`: `git fetch origin main && git pull origin main` (main/master 브랜치만 허용)
`workspace sync-all`: 모든 main 브랜치 순차 동기화

### IPC 액션

| 모듈 | 액션 | 설명 |
|------|------|------|
| workspace | scan | 수동 스캔 트리거 |
| workspace | list | 전체 로컬 브랜치 목록 |
| workspace | get | 단일 브랜치 상세 (params: `workspace_id`) |
| workspace | sync | 동기화 (params: `workspace_id` or `all: true`) |

### 설정

```toml
[workspace]
root = "D:/.workspace"            # 워크스페이스 루트
repo_name = "apex_pipeline"       # 스캔 접두어
scan_on_start = true              # 데몬 시작 시 자동 스캔
```

### Phase 2 예정: 세션 관리

`session_*` 필드는 Phase 2 (ConPTY + WebSocket 독립 프로세스)에서 활성화 예정:
- `MANAGED`: `apex-agent session` 프로세스가 ConPTY로 관리하는 세션
- `EXTERNAL`: CLI에서 직접 연 세션 (대시보드 읽기 전용)
- `STOP`: 세션 없음

설계서: `docs/apex_tools/plans/20260326_152349_workspace_session_mgmt.md`
