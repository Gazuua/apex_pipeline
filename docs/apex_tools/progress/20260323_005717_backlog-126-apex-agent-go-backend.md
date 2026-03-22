# BACKLOG-126: apex-agent Go 백엔드 완전 재작성

**브랜치**: `feature/backlog-126-apex-agent-go-backend`
**완료**: 2026-03-23

## 요약

11개 bash 스크립트(~2,080줄)로 구성되었던 에이전트 hook/자동화 시스템을 Go 단일 바이너리(14,000+ LOC)로 전면 재작성.

## 동기

- MSYS 경로 버그 반복 (#89, #90)
- grep+sed YAML 파싱 fragile
- 60+ 분기 상태머신의 bash 디버깅 한계
- 테스트 불가
- 스크립트 수 증가 부담

## 산출물

### 아키텍처

- **데몬 모드**: Named Pipe(Windows) / Unix Socket(Linux) IPC, SQLite WAL 상태 저장소
- **4개 모듈**: Hook Gate, Backlog 강타입 관리, Handoff 상태머신, Queue FIFO 빌드·머지 큐
- **5개 Hook 게이트**: validate-build, validate-merge, validate-handoff, enforce-rebase, handoff-probe
- **크로스 플랫폼**: `run-hook` bash 래퍼가 OS별 바이너리 자동 선택

### 코드 규모

| 구분 | LOC |
|------|-----|
| 프로덕션 코드 | ~7,100 |
| 테스트 코드 | ~6,900 |
| **합계** | **~14,000** |

### 삭제된 bash 스크립트 (5종)

- `branch-handoff.sh` (705줄)
- `cleanup-branches.sh` (374줄)
- `queue-lock.sh` (345줄)
- `session-context.sh` (126줄)
- `setup-claude-plugin.sh` (121줄)

### 테스트

- 14 Go 패키지 전체 PASS (단위 + E2E)
- CI Go 빌드+테스트 파이프라인 추가 (`.github/workflows/ci.yml`)

### 주요 커밋 (77개)

- Phase 0: 스캐폴딩 → IPC → Daemon → Store → Platform → Log
- Phase 1: Hook Gate, Backlog CRUD/Import/Export, Handoff 상태머신, Queue 모듈
- Phase 2: E2E 테스트 (8그룹 20+ 시나리오), CI 파이프라인
- Phase 3: Config, Logging, 버전 프로토콜, 시스템 설치
- Phase 4: run-hook 래퍼, bash 잔재 정리, benchmark 서브커맨드
- Phase 5: Backlog 강타입, export 안전장치, hook timeout 조정
- Auto-review 5라운드 수정 완료

## 연관 백로그 영향

- **#65** (auto-review 가이드 검증 자동화): #126 완료로 착수 가능 상태
- **#50** (스크립트 폴더 정리): bash 5종 삭제됨, 잔여 스크립트 재평가 필요
- **#59** (문서 자동화): Go 백엔드 안정화 완료, 재평가 가능
