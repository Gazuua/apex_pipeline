# cleanup 자기 워크트리 삭제 버그 핫픽스 + 핸드오프 구조 강화

**브랜치**: `bugfix/cleanup-self-destruct`
**백로그**: 신규 (기존 #146과 무관)
**작성일**: 2026-03-23

---

## 1. 배경

### 사고 경위

cleanup --execute 실행 시 현재 워크트리(branch_01)를 "머지 완료 브랜치"로 판단하고 디렉토리째 삭제.
원인: 빈 커밋 없이 main에서 생성된 브랜치를 `IsMergedToMain`이 "머지 완료"로 판정 + 현재 워크트리/핸드오프 상태 확인 로직 부재.

### 발견된 이슈

| # | 이슈 | 심각도 |
|---|------|--------|
| 1 | cleanup이 현재 워크트리를 삭제 | CRITICAL |
| 2 | 핸드오프 UNIQUE 충돌 — branches 레코드 미정리 | MAJOR |
| 3 | cleanup이 핸드오프 상태를 무시 | MAJOR |

이슈 3은 이슈 1의 해결책(cleanup–핸드오프 연동)에 포함되어 자동 해결.

---

## 2. 설계 결정

### 2.1 핸드오프 기반 브랜치 생성 (이슈 1 근본 해결)

**원칙**: 모든 브랜치는 핸드오프 등록과 원자적으로 생성된다. 핸드오프 없는 브랜치는 존재하지 않는다.

**흐름**:
```
notify start --branch-name feature/foo --backlog 146 --scopes core --summary "..."
│
├─ Phase 1: DB TX (원자적)
│   INSERT active_branches (status=STARTED, git_branch="feature/foo")
│   INSERT branch_backlogs
│   UPDATE backlog_items (OPEN → FIXING)
│   INSERT notifications
│   ── TX COMMIT ──
│
├─ Phase 2: Git (재시도 가능)
│   git checkout -b feature/foo
│   git push -u origin feature/foo
│
└─ 실패 복구:
    Phase 1 실패 → 재시도
    Phase 2 실패 → DB 레코드가 브랜치를 보호, git만 재시도
```

**DB 먼저 → git 후 순서의 안전성**:
- DB 레코드가 "브랜치 예약"으로 작동 — cleanup이 active_branches 조회 시 보호됨
- git 실패 시 DB 레코드가 잔존하지만, 다음 시도에서 git만 재실행하면 됨
- 보상 트랜잭션 불필요 (saga pattern 단순화)

**validate-build hook 확장** — 직접 브랜치 생성 차단:
- 차단: `git checkout -b`, `git switch -c`, `git switch --create`, `git branch <name>`
- 허용: `git branch`(조회), `git branch -D`(삭제), `git branch -a`, `git branch -v`
- 에러 메시지: `"차단: 직접 브랜치 생성 금지. 'apex-agent handoff notify start --branch-name <name>' 을 사용하세요."`

**notify start API 강화**:

| 파라미터 | 필수 | 검증 |
|----------|------|------|
| `--branch-name` | 필수 | `^(feature\|bugfix)/[a-z0-9][a-z0-9-]*$` 패턴 |
| `--summary` | 필수 | 비어있으면 거부 |
| `--scopes` | 필수 (격상) | 알려진 스코프 태그 검증 |
| `--backlog` | start에서 필수, job에서 불가 | 존재 + OPEN 상태 확인 |
| `--skip-design` | 선택 | - |

서버 측 추가 검증:
- branch-name이 active_branches에 이미 존재 → 거부
- branch-name이 로컬/리모트 git에 이미 존재 → 거부
- backlog가 FIXING 상태 → 거부
- scopes에 유효하지 않은 태그 → 거부

### 2.2 DB 스키마 변경 (이슈 2 해결)

**테이블 리네이밍**:
- `branches` → `active_branches` (활성 작업 레코드)
- 신규 `branch_history` (완료/포기 이력)

```sql
CREATE TABLE active_branches (
    branch      TEXT PRIMARY KEY,
    workspace   TEXT NOT NULL,
    git_branch  TEXT,
    status      TEXT NOT NULL,  -- STARTED, DESIGN_NOTIFIED, IMPLEMENTING
    summary     TEXT,
    created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);

CREATE TABLE branch_history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    branch      TEXT NOT NULL,
    workspace   TEXT NOT NULL,
    git_branch  TEXT,
    status      TEXT NOT NULL,  -- MERGED, DROPPED
    summary     TEXT,
    backlog_ids TEXT,            -- JSON 배열 스냅샷
    started_at  TEXT NOT NULL,
    finished_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
```

**마이그레이션 전략**:
- `ALTER TABLE branches RENAME TO active_branches`
- `CREATE TABLE branch_history`
- MERGE_NOTIFIED 레코드 → branch_history (status=MERGED) 이관 후 active_branches에서 삭제
- 나머지 전부 → branch_history (status=DROPPED) 이관 후 active_branches에서 삭제
- 현재 작업 브랜치(branch_agent)는 마이그레이션 후 재등록 필요

### 2.3 notify drop 신규 커맨드

```bash
apex-agent handoff notify drop --reason "작업 방향 변경"
```

**흐름**:
```
notify drop
│
├─ Pre-check: FIXING 백로그 잔존 확인
│   ├─ FIXING 존재 → 에러:
│   │   "FIXING 상태 백로그가 남아있습니다: #146, #147"
│   │   "먼저 backlog release 또는 backlog resolve 로 처리하세요"
│   │   → 실패 (exit 1)
│   └─ FIXING 없음 → 진행
│
├─ Phase 1: DB TX (원자적)
│   INSERT branch_history (status=DROPPED)
│   DELETE branch_backlogs
│   DELETE notification_acks (관련)
│   DELETE notifications (관련)
│   DELETE active_branches
│   ── TX COMMIT ──
│
└─ Phase 2: git checkout main && git pull origin main
```

**FIXING 게이트 통합**: notify merge와 notify drop의 FIXING 체크를 공용 함수로 추출.

### 2.4 notify merge 변경

기존 상태 전이(IMPLEMENTING → MERGE_NOTIFIED) 대신:
- active_branches → branch_history (status=MERGED) 이관
- Phase 2에 `git checkout main && git pull origin main` 추가
- FIXING 게이트는 기존 validate-merge-gate와 공용 함수 공유

### 2.5 cleanup 연동 (이슈 3 해결)

**cleanup.Run() 시그니처 변경**:
```go
func Run(repoRoot string, execute bool, activeBranches map[string]bool) (*Result, error)
```

각 Phase에 핸드오프 체크 추가:

| Phase | 기존 | 추가 |
|-------|------|------|
| Phase 1 (worktrees) | IsMergedToMain + isDirty | + activeBranches[wtBranch] → 스킵 |
| Phase 2 (local branches) | IsMergedToMain + dirtyBranches | + activeBranches[branch] → 스킵 |
| Phase 3 (remote branches) | IsMergedToMain | + activeBranches[branch] → 스킵 |

**activeBranches 조회**: CLI에서 데몬 IPC `handoff.list-active` 호출 → git_branch 목록 → map 구성.

**handoff 모듈에 list-active 라우트 추가**:
```go
"list-active" → SELECT branch, git_branch FROM active_branches → []{ branch, git_branch }
```

**CWD 보호 (defense-in-depth)**:
```go
// processWorktrees 내
cwd, _ := os.Getwd()
if isSubPath(cwd, wtPath) {
    // 경고 + 스킵
}
```

### 2.6 TxStore 타입 분리

현재 `Store`의 `tx *sql.Tx` 필드 패턴을 타입 레벨로 분리:

```go
// Store — DB 전용, 트랜잭션 시작만 담당
type Store struct {
    db *sql.DB
}
func (s *Store) RunInTx(ctx context.Context, fn func(tx *TxStore) error) error
func (s *Store) Exec/Query/QueryRow(...)  // 비트랜잭션 쿼리

// TxStore — 트랜잭션 전용, RunInTx 콜백 안에서만 존재
type TxStore struct {
    tx *sql.Tx
}
func (ts *TxStore) Exec/Query/QueryRow(...)  // 트랜잭션 쿼리
```

**영향 범위**: 전 모듈의 `RunInTx` 콜백과 `BacklogOperator.SetStatusWith` 시그니처 변경.

### 2.7 txs.Exec 에러 무시 버그 수정

`NotifyStart`의 재등록 로직(기존 레코드 정리)에서 `txs.Exec` 반환값을 무시하는 버그 수정.
모든 `Exec` 호출에 에러 체크 추가, 실패 시 트랜잭션 롤백.

---

## 3. 핸드오프 개입 전체 흐름

```
워크플로우 단계          핸드오프 개입               트랜잭션 묶음
═══════════════════════════════════════════════════════════════════

① 착수 ─────────── notify start ──────── TX {
                    (브랜치 생성 포함)       INSERT active_branches
                                            INSERT branch_backlogs
                                            UPDATE backlog_items (OPEN→FIXING)
                                            INSERT notifications
                                          }
                                          POST-TX { git checkout -b, git push -u }

② 설계 ─────────── notify design ─────── TX {
                                            UPDATE active_branches (→DESIGN_NOTIFIED)
                                            INSERT notifications
                                          }

③ 구현 ─────────── notify plan ────────── TX {
                                            UPDATE active_branches (→IMPLEMENTING)
                                            INSERT notifications
                                          }

④ 검증 ─────────── (변경 없음) ──────────  -
⑤ 리뷰 ─────────── (변경 없음) ──────────  -
⑥ 문서 갱신 ────── (변경 없음) ──────────  -

⑦ 머지 ─────────── notify merge ──────── TX {
                                            DELETE active_branches
                                            INSERT branch_history (MERGED)
                                            INSERT notifications
                                          }
                                          POST-TX { git checkout main, git pull }

✗ 포기 ──────────── notify drop ────────── Pre-check: FIXING 잔존 → 차단
                                          TX {
                                            DELETE active_branches
                                            INSERT branch_history (DROPPED)
                                            INSERT notifications
                                          }
                                          POST-TX { git checkout main, git pull }

═══════════════════════════════════════════════════════════════════
상시 Hook (PreToolUse)
═══════════════════════════════════════════════════════════════════
Bash:  validate-build ── cmake/ninja/build.bat + git checkout -b 차단
       validate-merge ── merge lock 미획득 시 차단
       validate-handoff ── 미등록 커밋 차단, 머지 시 미ack/FIXING 차단

Edit/Write:
       validate-handoff ── STARTED/DESIGN_NOTIFIED → 소스 차단
                           IMPLEMENTING → 허용
                           미등록 → 전체 차단
```

---

## 4. 변경 파일 목록

| 영역 | 변경 | 파일 |
|------|------|------|
| DB 스키마 | 리네이밍 + 신규 + 마이그레이션 | `modules/handoff/module.go` |
| TxStore | Store에서 TxStore 분리 | `store/store.go`, 전 모듈 |
| notify start | --branch-name, 검증, 브랜치 생성 | `modules/handoff/`, `cli/handoff_cmd.go` |
| notify drop | 신규 커맨드 | `modules/handoff/`, `cli/handoff_cmd.go` |
| notify merge/drop | Phase 2 git checkout main | `cli/handoff_cmd.go` |
| cleanup 연동 | active_branches 조회 + CWD 보호 | `cleanup/cleanup.go`, `cli/cleanup_cmd.go` |
| validate-build | git branch 차단 패턴 | `modules/hook/gate.go` |
| FIXING 게이트 | merge/drop 공용 함수 추출 | `modules/handoff/manager.go` |
| txs.Exec 버그 | 에러 체크 추가 | `modules/handoff/manager.go` |
| 백로그 | 5건 신규 등록 | `docs/BACKLOG.md` |
| 테스트 | 단위 + e2e | `cleanup/cleanup_test.go`, `e2e/` |

---

## 5. 백로그 등록 (이 PR 스코프 밖)

| 항목 | 등급 | 스코프 | 타입 |
|------|------|--------|------|
| `apex-agent git` CLI 서브커맨드 그룹 (checkout-main, switch, rebase) | MAJOR | TOOLS | INFRA |
| ModuleLogger `.With()` 매 호출 allocation 개선 | MINOR | TOOLS | PERF |
| Queue polling exponential backoff | MINOR | TOOLS | PERF |
| git 명령어 에러 핸들링 (rebase --abort 에러 무시) | MAJOR | TOOLS | BUG |
| Plugin 시스템 Claude Code 포맷 버전 체크 부재 | MINOR | TOOLS | INFRA |
