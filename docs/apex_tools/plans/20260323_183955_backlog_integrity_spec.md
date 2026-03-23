# 백로그 시스템 정합성 강화 설계

## 개요

백로그 DB↔MD 동기화의 엣지 케이스를 근본 해결하고, 모든 백로그 조작을 CLI 경유로 강제한다.

## 동기

이전 PR(#119)에서 발견된 문제들:
- `backlog add` 시 DB에 import 안 된 상태에서 ID 충돌
- `backlog export > docs/BACKLOG.md` 리다이렉션이 셸에서 파일 truncate
- `UpdateFromImport`가 title을 갱신하지 않아 오염 전파
- RESOLVED 항목이 BACKLOG_HISTORY.md에 자동 이관되지 않음
- 에이전트가 BACKLOG.md를 직접 편집하여 DB와 불일치 유발

## 원칙

- **DB가 Source of Truth** — 상태 + 메타데이터 모두
- **MD는 git 백업** — DB 유실 시 SyncImport로 복원
- **모든 백로그 조작은 CLI 경유** — MD 직접 편집 hook으로 차단

## 변경 사항

### 1. UpdateFromImport title 갱신 (BACKLOG-160)

**상태**: ✅ 이미 구현 완료 (이번 브랜치)

`manage.go`의 `UpdateFromImport()`에 `title` 파라미터 추가. import 시 MD 기준으로 title도 갱신.

### 2. backlog export 파일 직접 쓰기

현재: `backlog export` → stdout 출력 → 에이전트가 `> docs/BACKLOG.md` 리다이렉션
문제: 셸이 파일을 먼저 truncate → SafeExport의 import-first가 빈 파일 읽음

수정:
```bash
# 기본 동작: 파일 직접 쓰기
backlog export                    # → docs/BACKLOG.md + BACKLOG_HISTORY.md

# 디버깅용
backlog export --stdout           # → 기존처럼 stdout 출력
```

구현:
- `backlogExportCmd()`에서 기본 동작을 `SyncExport()` 호출로 변경
- `--stdout` 플래그 시 기존 stdout 출력 유지
- `SyncExport()`를 확장하여 HISTORY.md도 쓰도록 수정

### 3. HISTORY 자동 이관

export 시 RESOLVED 항목을 `docs/BACKLOG_HISTORY.md`에 자동 prepend.

구현:
- `backlog` 패키지에 `ExportHistory()` 함수 추가
  - DB에서 `status=RESOLVED` 항목 조회
  - 기존 HISTORY.md 읽기 → 기존 ID 목록 파악
  - HISTORY에 없는 RESOLVED 항목만 `<!-- NEW_ENTRY_BELOW -->` 마커 아래에 prepend
  - docs/CLAUDE.md의 히스토리 항목 템플릿 형식 준수
- `SyncExport()`에서 `ExportHistory()` 호출 추가

### 4. backlog update CLI

기존 `resolve`/`release` 유지. 메타데이터 수정 전용 커맨드 추가.

```bash
backlog update ID [--title "..."] [--severity MAJOR] [--timeframe IN_VIEW]
               [--scope TOOLS] [--type BUG] [--description "..."] [--related "150,151"]
```

- 지정된 필드만 갱신, 나머지 유지
- 최소 1개 필드 필수 (빈 호출 거부)
- DB 직접 접근 (daemon IPC 경유)

구현:
- `backlog_cmd.go`에 `backlogUpdateCmd()` 추가
- `manage.go`에 `Update()` 메서드 추가 — 지정된 필드만 SET하는 동적 SQL

### 5. validate-backlog hook

`docs/BACKLOG.md`와 `docs/BACKLOG_HISTORY.md`에 대한 Edit/Write를 무조건 차단.

구현:
- `settings.json`에 새 PreToolUse hook 등록
- `hook/gate.go`에 `ValidateBacklog()` 함수 추가
  - `tool_input.file_path`가 `docs/BACKLOG.md` 또는 `docs/BACKLOG_HISTORY.md`면 차단
  - 에러 메시지: "차단: BACKLOG 파일 직접 편집 금지. backlog add/update/resolve/release CLI를 사용하세요"
- 핸드오프 상태, 브랜치와 무관하게 무조건 차단

### 6. MergePipeline 순서 재배치

```
수정 전:
  ① RebaseOnMain
  ② SyncImport
  ③ SyncExport
  ④ IPC notify-merge

수정 후:
  ① RebaseOnMain
  ② SyncImport
  ③ SyncExport (BACKLOG.md + HISTORY.md)
  ④ git add + commit + push (export 결과, auto-commit)
  ⑤ CheckoutMain
  ⑥ IPC notify-merge (마지막)
```

IPC를 마지막으로 미루는 이유:
- ④⑤ 실패 시 active에 남아있음 → cleanup이 브랜치 보호 → 재시도 가능
- ⑤ 성공 → main에 있으므로 cleanup이 feature 삭제해도 무관
- ⑥ 실패 → main에 있고 active에 남아있음 → 재시도 가능
- 모든 단계 멱등 — 실패 시 전체 재실행으로 복구

getBranchID()는 workspace ID(디렉토리명) 기반이므로 checkout main 후에도 IPC 정상 동작.

④ auto-commit 메시지: `docs: backlog export (auto-sync)`

### 7. CLAUDE.md Source of Truth 보정

`apex_tools/apex-agent/CLAUDE.md`의 데이터 관리 섹션:

```
수정 전:
  MD: 메타데이터의 출처
  DB: 상태의 출처

수정 후:
  DB: Source of Truth (상태 + 메타데이터)
  MD: git 백업 (DB 유실 시 SyncImport로 복원)
```

## 파일 변경 목록

| 파일 | 변경 |
|------|------|
| `internal/modules/backlog/manage.go` | `Update()` 메서드 추가 |
| `internal/modules/backlog/export.go` | `ExportHistory()` 함수 추가 |
| `internal/modules/backlog/module.go` | update 라우트 등록 |
| `internal/cli/backlog_cmd.go` | `backlogUpdateCmd()` 추가, `backlogExportCmd()` 수정 |
| `internal/workflow/sync.go` | `SyncExport()` HISTORY 쓰기 추가 |
| `internal/workflow/pipeline.go` | MergePipeline 순서 재배치 (commit+push+checkout+IPC) |
| `internal/modules/hook/gate.go` | `ValidateBacklog()` 함수 추가 |
| `internal/cli/hook_cmd.go` | validate-backlog 서브커맨드 추가 |
| `.claude/settings.json` | validate-backlog hook 등록 |
| `apex_tools/apex-agent/CLAUDE.md` | Source of Truth 보정, export 사용법 갱신 |
| `CLAUDE.md` | 백로그 직접 편집 금지 규칙 추가 |

## 에러 처리

| 단계 | 실패 시 |
|------|--------|
| SyncExport | fatal — 파일 쓰기 실패, 머지 불가 |
| ExportHistory | fatal — HISTORY 쓰기 실패, 머지 불가 |
| git commit | 변경 없으면 no-op (정상) |
| git push | fatal — 리모트 반영 실패 |
| CheckoutMain | fatal — main 전환 실패, active 보호 유지, 재시도 |
| IPC notify-merge | fatal — DB finalize 실패, 재시도 |
| backlog update | 항목 미존재 시 에러, 열거형 검증 실패 시 에러 |
| validate-backlog | 무조건 차단 (exit 2) |

## 테스트 전략

- `backlog update`: 단위 테스트 — 필드별 개별 갱신, 열거형 검증, 항목 미존재
- `ExportHistory`: 단위 테스트 — 신규 RESOLVED prepend, 기존 항목 중복 방지, 마커 없을 때 복원
- `SyncExport` HISTORY: 단위 테스트 — round-trip (resolve → export → HISTORY 확인)
- `backlog export` CLI: 파일 직접 쓰기 검증, --stdout 기존 동작 유지
- MergePipeline: E2E — 순서 검증 (commit → checkout → IPC)
- validate-backlog: 단위 테스트 — BACKLOG.md 차단, 다른 파일 허용
