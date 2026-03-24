# 큐 히스토리 이벤트 로그 + 백로그 updated_at 버그 수정 설계

## 1. 개요

세 가지 이슈를 한 작업으로 처리한다:

1. **백로그 `updated_at` 버그**: import/export 시 모든 항목의 `updated_at`이 현재 시간으로 덮어써지는 문제 수정
2. **큐 히스토리 이벤트 로그**: 큐 상태 전이를 append 방식 이벤트 로그로 기록
3. **큐 페이지 레이아웃**: 빌드/머지 채널 좌우 분리 + 무한 스크롤 + 시간 범위 필터

## 2. 백로그 `updated_at` 버그 수정

### 현재 문제

`UpdateFromImport()` (manage.go:311)에서 `updated_at = datetime('now','localtime')` 하드코딩.
import할 때마다 모든 기존 항목의 타임스탬프가 현재 시간으로 갱신된다.

### 수정 방안

- Import 전 기존 DB 값을 조회
- `title`, `severity`, `timeframe`, `scope`, `type`, `description`, `related`, `position`, `status` 필드를 비교
- 하나라도 다르면 → `updated_at = datetime('now','localtime')` (실제 변경)
- 전부 동일하면 → `updated_at` 유지 (UPDATE 스킵 또는 원본 값 보존)

### 영향 범위

- `internal/modules/backlog/manage.go` — `UpdateFromImport()` 함수 1곳

## 3. 큐 히스토리 이벤트 로그

### 새 테이블: `queue_history`

```sql
CREATE TABLE queue_history (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    channel    TEXT NOT NULL,       -- "build" / "merge"
    branch     TEXT NOT NULL,
    status     TEXT NOT NULL,       -- "WAITING" / "ACTIVE" / "DONE"
    timestamp  TEXT NOT NULL        -- datetime('now','localtime')
);
```

마이그레이션: queue v4로 추가.

### 이벤트 기록 시점

기존 `queue` 테이블의 상태 전이 로직에 `queue_history` INSERT를 추가한다.

| 상황 | queue 테이블 동작 | queue_history INSERT |
|------|-------------------|---------------------|
| 경합 없음 — 바로 획득 | ACTIVE로 INSERT | `ACTIVE` 1건 |
| 경합 있음 — 대기 진입 | WAITING으로 INSERT | `WAITING` 1건 |
| 대기 → 획득 (promote) | status UPDATE → ACTIVE | `ACTIVE` 1건 |
| 완료 (release) | status UPDATE → DONE + finished_at | `DONE` 1건 |

경합이 없어서 WAITING을 거치지 않는 경우, WAITING 이벤트는 기록되지 않는다.

### Stale 엔트리 정리

`CleanupStale()`이 죽은 PID의 WAITING/ACTIVE 엔트리를 DELETE할 때, `queue_history`에는 기록하지 않는다.
DONE 이벤트 없이 ACTIVE/WAITING으로 끝난 히스토리는 비정상 종료를 의미한다.

### 기존 `queue` 테이블

변경 없이 유지. 현재 상태 관리 + 대시보드 메인 요약용.

## 4. 큐 전용 페이지 레이아웃

### 구조

```
┌──────────────────────────────────────────────────────────┐
│  [Today] [24h] [7d] | 📅 From: [datetime] To: [datetime] │
├────────────────────────────┬─────────────────────────────┤
│  Build Channel             │  Merge Channel              │
├────────────────────────────┼─────────────────────────────┤
│  10:05 feature/foo DONE    │  10:03 feature/bar ACTIVE   │
│  10:01 feature/foo ACTIVE  │  10:01 feature/bar WAITING  │
│  10:00 feature/foo WAITING │                             │
│  ...                       │  ...                        │
│  (스크롤 → 50개 추가)      │  (스크롤 → 50개 추가)       │
└────────────────────────────┴─────────────────────────────┘
```

### 필터

- **프리셋 버튼**: `Today` / `24h` / `7d`
- **커스텀 범위**: `<input type="datetime-local" step="1">` — 초 단위 지원, 외부 라이브러리 불필요
- 필터 미선택 시 기본: 최근 50개

### 무한 스크롤

- 각 채널 독립적으로 동작
- 초기 로드: 각 채널 최근 50개
- 바닥 도달 시: HTMX `hx-trigger="revealed"` → `offset` 증가하여 50개 추가 로드
- 더 이상 데이터 없으면 로더 숨김

### 실시간 폴링 + 무한 스크롤 공존

- 폴링은 최상단 영역만 갱신 — 새 이벤트를 상단에 prepend
- 이미 로드된 과거 이벤트는 유지
- 필터 변경 시 전체 리셋 후 재로드

### 대시보드 메인

기존 `queue_status.html` 유지 — ACTIVE/WAITING 요약만 표시 (변경 없음).

## 5. 조회 API

```
GET /partials/queue-history?channel={build|merge}&offset=0&limit=50[&from=...&to=...]
```

- `channel`: build / merge (필수)
- `offset` + `limit`: 무한 스크롤 페이지네이션
- `from`, `to`: ISO datetime 범위 필터 (선택)

## 6. 영향 범위 요약

| 파일 | 변경 내용 |
|------|-----------|
| `internal/modules/backlog/manage.go` | `UpdateFromImport()` 변경 감지 로직 추가 |
| `internal/modules/queue/module.go` | queue v4 마이그레이션 (queue_history 테이블) |
| `internal/modules/queue/manager.go` | 상태 전이 시 queue_history INSERT + DashboardHistory 조회 함수 |
| `internal/httpd/handler_queue.go` | history API 핸들러 추가 |
| `internal/httpd/server.go` | `QueueQuerier` 인터페이스에 `DashboardHistory()` 메서드 추가 |
| `internal/httpd/templates/queue.html` | 좌우 분리 레이아웃 |
| `internal/httpd/templates/partials/queue_page.html` | 히스토리 이벤트 목록 + 무한 스크롤 + 필터 UI |
| `internal/cli/httpd_adapters.go` | history 어댑터 추가 |
