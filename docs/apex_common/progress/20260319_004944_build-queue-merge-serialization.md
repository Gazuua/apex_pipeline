# #55 빌드 큐잉 + 머지 직렬화 시스템 — 완료 기록

## 요약

물리적으로 분리된 복수 브랜치 디렉토리(branch_01~03)가 동일 PC 자원을 공유할 때 빌드/머지 경합을 FIFO 큐로 직렬화하는 시스템 구축 완료.

## 산출물

| 파일 | 작업 | 설명 |
|------|------|------|
| `apex_tools/queue-lock.sh` | 신규 | 통합 큐/lock 스크립트 — FIFO 큐, mkdir atomic lock, PID/timestamp stale 감지, build/merge 채널 |
| `apex_tools/tests/test-queue-lock.sh` | 신규 | 기능 검증 테스트 (26개 assertion) |
| `.claude/hooks/validate-build.sh` | 신규 | PreToolUse hook — cmake/ninja/build.bat 직접 실행 차단 |
| `.claude/hooks/validate-merge.sh` | 신규 | PreToolUse hook — lock 미획득 상태에서 gh pr merge 차단 |
| `.claude/settings.json` | 수정 | PreToolUse hook 2개 등록 |
| `build.bat` | 수정 | EXTRA_ARGS 전달 지원 (--target 등) |
| `CLAUDE.md` | 수정 | 빌드 명령 queue-lock.sh 전환, 머지 프로세스 6단계 규칙 |

## 핵심 설계

- **단일 진입점**: `queue-lock.sh <채널> <서브커맨드>` — 채널 기반 확장 가능
- **FIFO 공정성**: 타임스탬프 기반 큐 파일로 선착순 보장
- **Atomic lock**: `mkdir` (NTFS atomic) — TOCTOU 방지
- **Stale 감지**: PID 생존 확인 (`kill -0` + `tasklist` fallback) + 타임스탬프 만료 (기본 60분)
- **다층 방어**: CLAUDE.md 규칙(soft) + PreToolUse hook(hard) + stale 감지(recovery)
- **토큰 절약**: 폴링 대기 메시지 최초 1회만 출력

## 검증

- 테스트 스크립트: 26/26 통과
- 빌드: 71/71 유닛 테스트 통과
- 코드 리뷰: CRITICAL 0건, 리뷰 피드백 3건 수정 완료
