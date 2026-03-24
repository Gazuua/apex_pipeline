# 큐 히스토리 이벤트 로그 + 백로그 updated_at 버그 수정

- **브랜치**: feature/queue-history-backlog-fix
- **PR**: #145
- **완료일**: 2026-03-24

## 작업 결과

### 1. 백로그 updated_at 버그 수정
- `UpdateFromImport()`에 변경 감지 로직 추가
- 모든 필드(title, severity, timeframe, scope, type, description, related, position, status) 비교
- 동일하면 UPDATE 스킵하여 원본 타임스탬프 보존

### 2. 큐 히스토리 이벤트 로그
- `queue_history` 테이블 신설 (마이그레이션 v4)
- 상태 전이(WAITING/ACTIVE/DONE)마다 이벤트 INSERT
- TryAcquire, Acquire, tryPromote, Release 모든 경로에서 기록
- Release를 RunInTx로 트랜잭션화 (TOCTOU race 방지)
- `DashboardHistory(channel, offset, limit, from, to)` 조회 함수

### 3. 큐 페이지 레이아웃 개편
- Build/Merge 채널 좌우 grid-2 분리
- 프리셋 필터 (Today/24h/7d) + 커스텀 datetime-local picker (초 단위)
- HTMX `revealed` 트리거 무한 스크롤 (50개 단위)
- 실시간 폴링으로 새 이벤트 상단 prepend
- 대시보드 메인 큐 위젯은 기존 ACTIVE/WAITING 요약 유지

### 4. auto-review 수정 (4건)
- offset/limit 클램핑 (음수/극값 방어)
- historyUrl에 url.QueryEscape 적용
- TestUpdateFromImport_NotFound 테스트 추가
- TestDashboardHistory_Empty, FromToFilter 테스트 추가

## 변경 파일
- Go: manage.go, manager.go, module.go, handler_queue.go, queries.go, render.go, routes.go, server.go, httpd_adapters.go, env.go
- 테스트: manage_test.go, manager_test.go, queries_test.go
- 템플릿: queue.html, queue_history.html (신규), queue_page.html (삭제)
- CSS: style.css
