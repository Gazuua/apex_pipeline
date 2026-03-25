# apex-agent 로직 정밀 강화

- **날짜**: 2026-03-25
- **PR**: #164
- **브랜치**: feature/agent-logic-hardening

## 작업 요약

다중 에이전트 환경에서 워크플로우 신뢰성 + DB 정합성을 보장하기 위한 정밀 강화.

### 변경 항목

1. **StartPipeline SyncImport 제거** — import는 백업 복원 전용으로 한정, 착수 단계 체크 불필요
2. **fail-close 전환** — validate-merge, validate-handoff, handoff-probe 데몬 down 시 차단 (기존 fail-open). resolveHandoffBranch IPC 에러도 fail-close
3. **branch_backlogs 소유권 정리** — backlog.Fix() 직접 INSERT → junctionCreator 콜백 패턴 통일. backlog 모듈이 handoff 테이블 구조를 알 필요 없음
4. **CleanupStale TX 감싸기** — PID check + DELETE를 단일 트랜잭션으로 원자성 확보. defer rows.Close() 추가
5. **SetStatusWith 에러 메시지 개선** — 동시 착수 시 "already FIXING (possibly concurrent)" 진단 메시지
6. **워크플로우 가이드 문서** — 7단계별 기능 매핑, 상태 전이 테이블, Hook 게이트 매트릭스, 동시성 시나리오

### auto-review 수정

- resolveHandoffBranch IPC 에러 fail-close
- CleanupStale defer rows.Close()
- gh pr merge 감지 containsShellCommand 통일
- enforce-rebase os.Getwd() 에러 처리
