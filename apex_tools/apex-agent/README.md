# apex-agent

Apex Pipeline 개발 자동화 백엔드. Claude Code hook 시스템, 백로그 관리, 브랜치 핸드오프를 단일 Go 바이너리로 제공한다.

## 아키텍처

```
apex-agent (CLI)
  ├─ daemon run/start/stop    — 백그라운드 데몬 관리
  ├─ hook validate-*          — Claude Code PreToolUse hook 게이트
  ├─ backlog add/list/resolve — 백로그 CRUD
  ├─ handoff notify/ack/check — 브랜치 핸드오프 상태 머신
  └─ queue acquire/release    — 빌드/머지 큐 잠금

데몬 (Named Pipe IPC)
  ├─ hook 모듈     — 빌드 경로/머지 잠금 검증
  ├─ backlog 모듈  — SQLite CRUD + enum 검증
  ├─ handoff 모듈  — 상태 머신 + junction 테이블 + 게이트
  └─ queue 모듈    — 빌드/머지 직렬화 큐
```

## 빌드

```bash
# Makefile (bash/MSYS)
make build          # 로컬 빌드
make test           # 테스트 (race detector)
make install        # $LOCALAPPDATA/apex-agent/ 에 설치

# Windows 더블클릭
build.bat           # 빌드
build.bat test      # 테스트
build.bat install   # 설치
```

## Hook 실행

Claude Code hook은 `run-hook` bash 래퍼를 통해 크로스 플랫폼으로 실행된다.
Windows에서는 `apex-agent.exe`, Linux에서는 `apex-agent`를 자동 선택.

```json
"command": "bash ./apex_tools/apex-agent/run-hook hook validate-build"
```

## 디렉토리 구조

```
cmd/apex-agent/       진입점
internal/
  cli/                CLI 커맨드 + hook 핸들러
  daemon/             데몬 코어 (IPC 서버, 모듈 등록)
  ipc/                Named Pipe / Unix Socket 프로토콜
  modules/
    backlog/          백로그 CRUD + enum 검증 + import/export
    handoff/          핸드오프 상태 머신 + 게이트 + junction
    hook/             빌드 경로 차단 + 머지 잠금 + 리베이스
    queue/            빌드/머지 큐 잠금
  store/              SQLite 래퍼 + 마이그레이션
  platform/           OS별 경로/프로세스 유틸
  config/             TOML 설정 로더
  log/                구조적 로깅
e2e/                  E2E 통합 테스트
  testenv/            테스트 환경 (인프로세스 데몬)
```

## 주요 기능

### 백로그 강타입 시스템
- 열거형 필드(severity, type, scope, status, resolution) 코드 레벨 검증
- `UPPER_SNAKE_CASE` 강제 — 소문자/레거시 값 자동 거부
- DB 마이그레이션으로 기존 데이터 자동 정규화

### 핸드오프 시스템
- 상태 머신: `started → design-notified → implementing → merge-notified`
- `branch_backlogs` junction 테이블로 복수 백로그 연결
- `git_branch` 컬럼 fallback — workspace ID 불일치 시 git branch name으로 조회
- 머지 게이트: 미ack 알림 + FIXING 백로그 차단

### Hook 게이트
- `validate-build`: cmake/ninja 직접 호출 차단 → queue-lock.sh 강제
- `validate-handoff`: 미등록 커밋 차단, 미해결 백로그 머지 차단
- `handoff-probe`: 상태별 소스 파일 편집 제어
- `enforce-rebase`: push 전 자동 리베이스
