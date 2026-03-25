# SetStatusWith RESOLVED 원복 방지 + README SVG 경로 수정

- **날짜**: 2026-03-25
- **PR**: #162
- **브랜치**: bugfix/backlog-import-status-readme-svg

## 작업 요약

### 근본 원인 분석

백로그 #203이 Resolve 후에도 OPEN으로 남아있던 원인을 데몬 로그 추적으로 발견.

**버그 체인**: `notify start` (브랜치 재등록) → stale entry 정리 → `SetStatusWith(id, "OPEN")` → RESOLVED 항목도 무차별 OPEN 전이.

`SetStatusWith`에 OPEN 전이 시 현재 status 확인 가드가 없어서, FIXING이 아닌 RESOLVED 상태에서도 OPEN으로 되돌려짐.

### 수정 내용

1. **SetStatusWith OPEN 가드** (`manage.go`): OPEN 전이 시 `AND status = 'FIXING'` WHERE 조건 추가 — RESOLVED→OPEN DB 레벨 차단
2. **NotifyStart stale cleanup** (`handoff/manager.go`): SetStatusWith 에러 시 info 로그로 스킵 (트랜잭션 중단 방지)
3. **NotifyDrop finalizeBranch** (`handoff/manager.go`): 동일 패턴 수정 (auto-review 발견)
4. **UpdateFromImport status 제거** (`manage.go`, `import.go`): import 경로에서 status 변경 불가하도록 시그니처 리팩터
5. **README.md SVG 경로** : BACKLOG-214 폴더 이동 후 누락된 경로 갱신
6. **단위 테스트 2건 추가** (`manage_test.go`): RESOLVED→OPEN 차단 + FIXING→OPEN 정상 경로

### 전수 조사

OPEN 상태에서 resolution이 설정된 백로그 전수 조사 → #203이 유일. 정리 완료.

### 백로그

- BACKLOG-221: SetStatusWith/handoff mock 정합성 (auto-review 발견, MINOR)
