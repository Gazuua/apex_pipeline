# 백로그 정합성 강화 — 완료 기록

**브랜치**: `bugfix/backlog-sync-fixes` (BACKLOG-160)
**일시**: 2026-03-23

---

## 구현 내용

### 핵심 기능 (8태스크)

1. **ExportHistory** — DB RESOLVED 항목을 BACKLOG_HISTORY.md에 자동 prepend. 중복 방지(parseHistoryIDs), 마커 누락 시 자동 복원
2. **SyncExport HISTORY** — SyncExport에 ExportHistory 호출 추가, BACKLOG.md + BACKLOG_HISTORY.md 동시 갱신
3. **backlog export CLI** — 기본 동작을 파일 직접 쓰기로 변경 (`--stdout` 디버깅용)
4. **backlog update CLI** — 필드별 개별 메타 수정 (8개 플래그: title, severity, timeframe, scope, type, description, related, position)
5. **validate-backlog hook** — docs/BACKLOG.md, BACKLOG_HISTORY.md 직접 편집 무조건 차단 (대소문자 무관)
6. **MergePipeline 순서 재배치** — rebase→import→export→autoCommitExport→checkoutMain→IPC(마지막)
7. **CLAUDE.md 보정** — SoT를 DB 중심으로 보정, hook 테이블 갱신, 백로그 직접 편집 금지 규칙
8. **position 재배치** — `backlog update --position N`: 같은 timeframe 내 항목 자동 shift

### 리뷰 수정 (5건)

- ExportHistory off-by-one panic 방어
- Update() scope 검증 누락 추가
- autoCommitExport git add 에러 반환
- SyncExport history 이중 읽기 제거
- ValidateBacklog 대소문자 무관 매칭

### BACKLOG-160 직접 수정

- `UpdateFromImport`에 title 파라미터 추가 (메타데이터 갱신 누락 버그)

---

## 변경 파일

| 영역 | 파일 | 변경 |
|------|------|------|
| backlog | export.go | ExportHistory, parseHistoryIDs, writeHistoryItem |
| backlog | export_test.go | 7개 테스트 추가 |
| backlog | manage.go | Update(), allowedUpdateFields, position reorder |
| backlog | manage_test.go | 9개 테스트 추가 |
| backlog | import.go | UpdateFromImport title 파라미터 |
| backlog | module.go | handleUpdate 라우트 |
| workflow | pipeline.go | MergePipeline 6-phase, autoCommitExport |
| workflow | sync.go | SyncExport HISTORY 쓰기 |
| workflow | sync_test.go | 1개 테스트 추가 |
| cli | backlog_cmd.go | export/update 커맨드 |
| cli | hook_cmd.go | validate-backlog 서브커맨드 |
| cli | handoff_cmd.go | 안내 메시지 제거 |
| hook | gate.go | ValidateBacklog (대소문자 무관) |
| hook | gate_test.go | 6개 테스트 추가 |
| config | settings.json | validate-backlog hook 등록 |
| docs | CLAUDE.md (루트) | 백로그 직접 편집 금지 규칙 |
| docs | apex-agent/CLAUDE.md | SoT 보정, hook 테이블, CLI 사용법 |
| docs | BACKLOG.md | export 동기화 |
| docs | BACKLOG_HISTORY.md | RESOLVED 항목 이관 |

## 테스트

- Go 전체 테스트: ALL PASS (0 FAIL)
- 바이너리 설치 + 스모크 테스트: 정상
