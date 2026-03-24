# IPC ctx 전파 + client 재사용 + Dispatcher 통합 설계

## 1. 개요

세 가지 DESIGN_DEBT 이슈를 한 작업으로 처리한다:

1. **BACKLOG-194**: Dispatcher 인터페이스 이중 정의 통합
2. **BACKLOG-189**: IPC 핸들러 ctx 전파 — graceful shutdown + request timeout 지원
3. **BACKLOG-193**: IPC client 매 호출 재생성 — versionOnce 무의미화

### 실행 순서

194 (Dispatcher 통합) → 189 (ctx 전파) → 193 (client 재사용)

- 194로 공유 패키지를 먼저 만들어 기반 정리
- 189는 가장 파급 범위가 넓으므로 깨끗한 상태에서 진행
- 193은 1파일 수정으로 마지막에 붙임

## 2. Dispatcher 인터페이스 통합 (BACKLOG-194)

### 현재 문제

`httpd/server.go`와 `ipc/server.go`에서 동일한 Dispatcher 인터페이스를 각각 독립 정의. 시그니처 변경 시 양쪽 동시 수정 필요.

### 해결

새 경량 패키지 `internal/dispatch/dispatch.go` 생성:

```go
package dispatch

import (
    "context"
    "encoding/json"
)

// Dispatcher routes requests to module handlers.
type Dispatcher interface {
    Dispatch(ctx context.Context, module, action string, params json.RawMessage, workspace string) (any, error)
}
```

변경:
- `httpd/server.go`: 로컬 Dispatcher 인터페이스 삭제 → `dispatch.Dispatcher` 사용
- `ipc/server.go`: 로컬 Dispatcher 인터페이스 삭제 → `dispatch.Dispatcher` 사용
- `daemon/router.go`: 변경 없음 (이미 동일 시그니처 구현)

Import 방향: `httpd` → `dispatch` ← `ipc` (cycle 없음)

## 3. Store ctx 전파 (BACKLOG-189)

### 현재 문제

- `Querier` 인터페이스의 `Exec/Query/QueryRow`에 context 없음
- 대부분의 IPC 핸들러가 ctx를 `_`로 무시
- graceful shutdown, request timeout이 DB 쿼리까지 전파되지 않음

### 해결

#### Querier 인터페이스 변경

```go
type Querier interface {
    Exec(ctx context.Context, query string, args ...any) (sql.Result, error)
    Query(ctx context.Context, query string, args ...any) (*sql.Rows, error)
    QueryRow(ctx context.Context, query string, args ...any) *sql.Row
}
```

#### Store/TxStore 구현 변경

- `Store.Exec` → `s.db.ExecContext(ctx, query, args...)`
- `Store.Query` → `s.db.QueryContext(ctx, query, args...)`
- `Store.QueryRow` → `s.db.QueryRowContext(ctx, query, args...)`
- `TxStore`도 동일하게 `tx.ExecContext/QueryContext/QueryRowContext` 사용

#### 전파 경로

```
IPC 요청 → HandlerFunc(ctx, params, workspace)
                ↓ ctx 전달
         Manager.Method(ctx, ...)
                ↓ ctx 전달
         store.Exec(ctx, query, args...)
                ↓
         db.ExecContext(ctx, ...)
```

#### 핸들러 수정

현재 `_`로 ctx를 무시하는 핸들러에서 ctx를 받아 Manager로 전달:
- backlog: 전체 핸들러 (add, list, get, resolve, check, next-id, export, release, update, fix, sync-import)
- handoff: get-branch, get-status, resolve-branch, validate-* 핸들러
- queue: release, status, update-pid, cleanup-stale 핸들러
- hook: validate-merge 핸들러

#### Manager 메서드 ctx 추가

Manager 메서드에 `ctx context.Context` 첫 파라미터 추가. 내부에서 `m.q.Exec(ctx, ...)` 형태로 전파.

#### Dashboard 쿼리 ctx 전략

Dashboard 메서드(`DashboardStatusCounts`, `DashboardActiveBranches`, `DashboardQueueAll` 등)는 `context.Background()` 사용.
근거: HTTP 핸들러의 요청 컨텍스트는 HTTP 서버가 관리하고, dashboard 쿼리는 짧은 읽기 전용이므로 ctx 전파 ROI가 낮음.
따라서 `httpd.BacklogQuerier`/`HandoffQuerier`/`QueueQuerier` 인터페이스는 변경하지 않음.

#### BacklogOperator 인터페이스 변경

`handoff/manager.go`의 `BacklogOperator` 인터페이스도 ctx 추가:

```go
type BacklogOperator interface {
    SetStatus(ctx context.Context, id int, status string) error
    SetStatusWith(ctx context.Context, q store.Querier, id int, status string) error
    Check(ctx context.Context, id int) (exists bool, status string, err error)
    ListFixingForBranch(ctx context.Context, branch string, backlogIDs []int) ([]int, error)
}
```

#### handoff gate 메서드

`handoff/gate.go`의 `ValidateCommit`, `ValidateMergeGate`, `ValidateEdit` 메서드에 ctx 추가.
내부에서 호출하는 `GetStatus`, `checkFixingBacklogs` 등에도 ctx 전파.

#### 의도적 context.Background() 유지

`queue/manager.go`의 `tryPromote`는 의도적으로 `context.Background()` 사용 (기존 주석 참조: "Background intentionally — short transaction, cancel cleanup handled by ctx.Done() path above"). 이 패턴은 유지.

#### 비-핸들러 호출 사이트

마이그레이션(`migrator.go`), 테스트 헬퍼, daemon 초기화(`daemon_cmd.go` junction cleaner lambda) 등 핸들러 밖에서 호출되는 곳은 `context.Background()` 사용. `Store.Open`의 PRAGMA 호출은 `*sql.DB.Exec` 직접 호출이므로 Querier 인터페이스 변경에 영향 없음.

## 4. IPC client 재사용 (BACKLOG-193)

### 현재 문제

`ipc_helpers.go`의 `sendRequest`가 매번 `ipc.NewClient` 생성 → `versionOnce`가 인스턴스별이라 `checkVersion` 매번 실행.

### 해결

패키지 레벨 `sync.Once`로 단일 Client 인스턴스 관리:

```go
var (
    defaultClient     *ipc.Client
    defaultClientOnce sync.Once
)

func getClient() *ipc.Client {
    defaultClientOnce.Do(func() {
        defaultClient = ipc.NewClient(platform.SocketPath())
    })
    return defaultClient
}
```

`sendRequest`와 `sendRequestWithTimeout`이 `getClient()`를 사용. CLI 프로세스 수명이 짧으므로 소켓 경로 변경 걱정 없음.

## 5. 영향 범위 요약

### 소스 파일

| 파일 | 변경 내용 |
|------|-----------|
| `internal/dispatch/dispatch.go` (신규) | Dispatcher 인터페이스 정의 |
| `internal/httpd/server.go` | 로컬 Dispatcher 삭제 → dispatch.Dispatcher |
| `internal/ipc/server.go` | 로컬 Dispatcher 삭제 → dispatch.Dispatcher |
| `internal/store/store.go` | Querier/Store/TxStore에 ctx 추가 |
| `internal/store/migrator.go` | Exec/QueryRow 호출에 context.Background() 추가 |
| `internal/modules/backlog/module.go` | 핸들러 ctx 전달 |
| `internal/modules/backlog/manage.go` | Manager 메서드 ctx 추가 |
| `internal/modules/backlog/export.go` | store 호출에 ctx 추가 |
| `internal/modules/backlog/import.go` | store 호출에 ctx 추가 |
| `internal/modules/handoff/module.go` | 핸들러 ctx 전달 |
| `internal/modules/handoff/manage.go` | Manager 메서드 ctx 추가 + BacklogOperator 인터페이스 변경 |
| `internal/modules/handoff/gate.go` | Validate* 메서드 ctx 추가 |
| `internal/modules/queue/module.go` | 핸들러 ctx 전달 |
| `internal/modules/queue/manager.go` | Manager 메서드 ctx 추가 (tryPromote는 Background 유지) |
| `internal/modules/hook/module.go` | 핸들러 ctx 전달 |
| `internal/cli/ipc_helpers.go` | 패키지 레벨 client + getClient() |
| `internal/cli/daemon_cmd.go` | junction cleaner lambda에 ctx 추가 |
| `internal/cli/migrate_cmd.go` | Manager 호출에 ctx 추가 |

### 테스트 파일

| 파일 | 변경 내용 |
|------|-----------|
| `internal/store/store_test.go` | Exec/Query/QueryRow에 context.Background() 추가 |
| `internal/store/migrator_test.go` | 동일 |
| `internal/modules/backlog/manage_test.go` | Manager 메서드 + store 호출에 ctx 추가 |
| `internal/modules/backlog/export_test.go` | 동일 |
| `internal/modules/backlog/import_test.go` | 동일 |
| `internal/modules/handoff/manager_test.go` | Manager 메서드 + store 호출에 ctx 추가 |
| `internal/modules/handoff/gate_test.go` | Validate* 메서드에 ctx 추가 |
| `internal/modules/queue/manager_test.go` | Manager 메서드 + store 호출에 ctx 추가 |
| `internal/httpd/queries_test.go` | setupTestStore의 store 호출에 ctx 추가 |
| `internal/workflow/pipeline_test.go` | store 호출에 ctx 추가 |
| `internal/workflow/sync_test.go` | store 호출에 ctx 추가 |
| `e2e/testenv/env.go` | store/module 호출에 ctx 추가 |
