# apex-agent Go 코드베이스 Full Auto-Review 완료

**일시**: 2026-03-26
**브랜치**: `feature/apex-agent-auto-review`
**PR**: #182

## 작업 내용

apex-agent Go 백엔드 전체(115 파일, ~22K LOC) 대상 full auto-review.
5개 리뷰어(logic, design, test, infra-security, systems)를 병렬 디스패치하여 리뷰 후, 발견 이슈 21건을 즉시 수정.

## 수정 요약

| 심각도 | 건수 | 대표 항목 |
|--------|:----:|----------|
| CRITICAL | 1 | watchdog killed 플래그 이중 목적 버그 |
| MAJOR | 8 | rows.Err() 미검사, RowsAffected() 에러 무시, DataDir 방어, E2E Restart |
| MINOR | 11 | 로그 추가, CLI 출력 개선, 테스트 보강 |
| 설계 개선 | 1 | ImportFn/ExportFn context 인자 주입 통일 |

## 변경 파일 (17개)

- `internal/cli/queue_cmd.go`, `queue_cmd_test.go` — watchdog 분리
- `internal/modules/backlog/module.go` — migration rows.Err()
- `internal/modules/queue/manager.go` — RowsAffected() 에러 체크
- `internal/platform/paths.go` — DataDir 방어, NormalizePath 검증
- `internal/config/config.go` — WriteDefault 에러 처리
- `internal/modules/hook/rebase.go` — 경고 로그 추가
- `internal/cli/backlog_cmd.go` — STATUS 컬럼, BACKLOG-N 형식
- `internal/cli/context_cmd.go` — RunE 패턴 통일
- `internal/workflow/pipeline.go`, `pipeline_test.go` — context 인자 주입
- `internal/modules/handoff/module.go` — context 캡처 제거
- `e2e/testenv/env.go` — Restart HTTP 설정
- `internal/httpd/queries_test.go` — defer 패턴
- `internal/store/store_test.go` — Scan 에러 체크
- `internal/modules/backlog/import_test.go` — Migrate 에러 체크
- `e2e/backlog_test.go` — 에러 메시지 수정

## 빌드 결과

- 로컬: `go test ./...` 전 패키지 PASS, `go build` 성공
- CI: PR #182에서 검증 중
