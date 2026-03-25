# 데몬 idle timeout 제거 + IPC auto-restart (BACKLOG-231)

**날짜**: 2026-03-25
**브랜치**: feature/daemon-idle-timeout-removal
**PR**: #169

## 배경

데몬의 30분 idle timeout이 fail-close hook 전략과 결합하여, 설계 논의 등 장시간 비활동 후 전체 도구가 차단되는 문제.

## 변경 내용

### 1. idle timeout 제거
- `daemon.Config.IdleTimeout` 필드 삭제
- `config.DaemonConfig.RawIdleTimeout` 필드 삭제
- daemon Run()의 idle ticker select 루프 → 단순 shutdown 대기 select로 교체
- config.toml WriteDefault에서 `idle_timeout` 행 삭제

### 2. IPC auto-restart
- `sendWithAutoRestart()`: dial 에러 감지 → `ensureDaemon()` → 1회 재시도
- `sendRequest()`와 `sendRequestWithTimeout()` 모두 자동 적용
- `isDialError()`: `"dial:"` 접두사로 연결 실패 판별

### 3. hook 바이패스
- `isDaemonManagementCommand()`: `daemon start/stop/status/run`, `plugin setup` 명령 감지
- validate-handoff hook 진입부에서 조기 반환
- `isGitCommit()` 체크로 커밋 메시지 내 false positive 방지

### 4. maintenance lock
- `daemon stop` → `apex-agent.maintenance` 파일 생성
- `ensureDaemon()` → maintenance 파일 존재 시 auto-restart 스킵
- `daemon start/run` → maintenance 파일 삭제
- `daemon stop` 완전 실패 시 maintenance 파일 정리

## 테스트

- Go 전체 테스트 통과 (단위 + E2E)
- `TestIsDaemonManagementCommand`: 14개 케이스 (정상 바이패스, git commit false positive, 체인 명령)
- `TestIsDialError`: 5개 케이스
- 실제 daemon stop → build → install → start 체인 동작 확인
