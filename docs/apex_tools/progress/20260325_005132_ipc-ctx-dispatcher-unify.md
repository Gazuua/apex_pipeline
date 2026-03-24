# IPC ctx 전파 + Dispatcher 통합 + IPC client 재사용

- **브랜치**: feature/ipc-ctx-dispatcher-unify
- **PR**: #150
- **완료일**: 2026-03-25

## 작업 결과

### BACKLOG-194: Dispatcher 인터페이스 통합
- `internal/dispatch/dispatch.go` 신규 패키지 생성
- httpd/server.go, ipc/server.go의 로컬 Dispatcher 정의 삭제 → 공유 인터페이스 사용
- import cycle 없음 확인

### BACKLOG-189: Store ctx 전파
- `Querier` 인터페이스의 Exec/Query/QueryRow에 `ctx context.Context` 첫 인자 추가
- Store/TxStore 구현이 ExecContext/QueryContext/QueryRowContext 사용
- 모든 IPC 핸들러에서 ctx를 Manager로 전달, Manager가 store까지 전파
- BacklogOperator 인터페이스에도 ctx 추가
- Dashboard 메서드는 context.Background() 사용 (짧은 읽기 전용)
- tryPromote는 의도적 context.Background() 유지

### BACKLOG-193: IPC client 재사용
- `ipc_helpers.go`에 `sync.Once` 기반 싱글톤 `getClient()` 도입
- `sendRequest`/`sendRequestWithTimeout`이 매번 새 Client 생성 → 공유 인스턴스 사용
- `versionOnce`가 프로세스당 한 번 실행되도록 수정

## 변경 통계
- 34개 파일, +737 / -644 줄
- 서브에이전트 5명 병렬 실행 (store, backlog, handoff, queue, remaining)
