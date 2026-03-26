# apex-agent 데몬 로그 일별 분할 + 워크플로우 로그 보강

- **PR**: #167
- **브랜치**: feature/agent-daily-log-rotation
- **상태**: 완료

## 변경 요약

### DailyWriter (신규)
- `internal/log/daily_writer.go`: `logs/YYYYMMDD.log` 형태로 일별 파일 회전하는 `io.Writer` 구현
- `max_days` 설정으로 오래된 로그 파일 자동 정리 (기본 30일)
- lumberjack.v2 의존성 완전 제거

### 설정 변경
- `config.go`: `MaxDays` 필드 추가 (기본 30일), `MaxSizeMB`/`MaxBackups` deprecated
- `daemon_cmd.go`: lumberjack → DailyWriter 교체, `logs/` 서브디렉토리 사용
- 로그 경로: `$LOCALAPPDATA/apex-agent/logs/YYYYMMDD.log`

### 워크플로우 로그 보강 (10개 모듈)
- **handoff**: NotifyStart/Transition/Merge/Drop 시작·완료, backlog 사전 검증, ResolveBranch 조회 경로
- **queue**: TryAcquire/Acquire/Release/UpdatePID/CleanupStale 상세, dead PID 감지
- **backlog**: Add/Resolve/Release/Fix/Update 시작, import stale 스킵
- **hook**: ValidateBuild/ValidateBacklog 디버그 입력
- **cleanup**: 4단계 phase 시작·완료, 결과 통계
- **workflow**: ValidateNewBranch/CreateAndPushBranch/RebaseOnMain/CheckoutMain 시작·완료·실패
- **daemon**: 초기화 상세, 마이그레이션, 모듈 시작/종료 시퀀스, graceful shutdown
- **ipc**: IPC 서버 시작, 요청 처리 시간(elapsed_ms), 실패 경고
- **context**: 세션 컨텍스트 생성 info 승격
- **store**: DB open/close 로그

### 테스트
- DailyWriter 유닛 테스트 5건 추가 (파일 생성, 날짜 전환, 오래된 파일 정리, Close, 비날짜 파일 보존)
- 전체 Go 테스트 통과 (e2e 포함)

## 변경 파일 (16건)
- 신규: `daily_writer.go`, `daily_writer_test.go`
- 수정: `go.mod`, `go.sum`, `daemon_cmd.go`, `config.go`, `daemon.go`, `server.go`, `manage.go`, `manager.go` (handoff), `gate.go`, `manager.go` (queue), `store.go`, `git.go`, `cleanup.go`, `context.go`
