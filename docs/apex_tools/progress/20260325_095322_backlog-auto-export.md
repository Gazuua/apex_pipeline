# backlog CLI 자동 export + stale import guard

- **날짜**: 2026-03-25
- **브랜치**: `bugfix/backlog-auto-export`
- **PR**: #156
- **백로그**: BACKLOG-215

## 배경

`backlog update` CLI가 DB만 갱신하고 `docs/BACKLOG.json` export를 수행하지 않아,
이후 다른 명령의 import-first 안전장치가 JSON의 옛 데이터를 DB에 덮어써서 update가 원복되는 버그 발견.
추가로 브랜치 간 JSON 버전 차이에 의한 cross-branch stale import 문제도 식별.

## 구현

1. **autoExport()** 헬퍼 — CLI mutation(add/update/resolve/release/fix) 성공 후 자동 JSON export
2. **stale import guard** — `UpdateFromImport`에서 DB의 `updated_at`이 import 데이터보다 새로우면 스킵 (DB wins)
3. **테스트** — StaleGuard + EmptyTimestamp 2건 추가, 기존 3건 시그니처 수정

## 검증

- `go test ./... -count=1` 전체 PASS
- 실제 `backlog update` 후 auto-export 동작 확인
- stale import 시뮬레이션 (옛 JSON 복원 → export → DB title 유지) 확인
