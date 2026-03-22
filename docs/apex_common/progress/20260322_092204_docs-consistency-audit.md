# 문서 정합성 감사

**날짜**: 2026-03-22
**브랜치**: feature/docs-consistency-audit
**타입**: docs

## 요약

BACKLOG.md, BACKLOG_HISTORY.md, README.md, apex_core_guide.md 간 정합성 정밀 검증 및 수정.

## 변경 내역

### BACKLOG.md

| 항목 | 조치 | 사유 |
|------|------|------|
| #49, #121, #122 | IN VIEW에서 제거 | BACKLOG_HISTORY에 이미 FIXED 기록됨 (2026-03-21). 잔류 오류 |
| #123 → #127 재발번 | ID 충돌 해소 | 기존 #123은 "FSD Backlog 자동화"로 HISTORY에 확정 (커밋 4009baf). blacklist 테스트 항목이 동일 ID를 오사용 |
| #38 | DEFERRED에서 제거 | Boost.Beast가 v0.5.0.0에서 WebSocket 통합 완료. websocket_protocol.hpp에서 실사용 중 |
| #65 연관 필드 | `#1` → `#1 (HISTORY)` | 참조 대상이 히스토리에 있음을 명시 |
| 다음 발번 | 127 → 128 | #127 발번으로 인한 카운터 갱신 |

### BACKLOG_HISTORY.md

- #38: SUPERSEDED로 등록 (v0.5.0.0 WebSocket 통합으로 해소)
- #63: 중복 항목 제거 (2026-03-21 22:16:17 항목, 2026-03-22 00:56:28 항목과 동일)

### README.md

- v0.5.10.1 보안 시크릿 관리 섹션 추가 (누락됨)
- v0.5.9.0 "forwarding header 유지, 로드맵 진행 중" → "deprecated → 직접 include 전환 완료"로 갱신

### apex_core_guide.md

- 헤더 버전 v0.5.10.0 → v0.5.10.2 (내용은 이미 최신 반영됨, 버전 표기만 누락)
