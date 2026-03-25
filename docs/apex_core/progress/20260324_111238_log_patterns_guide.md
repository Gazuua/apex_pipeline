# 로그 패턴 가이드 문서 작성 완료

**일자**: 2026-03-24
**브랜치**: feature/log-patterns-guide
**버전**: v0.5.11.0

## 작업 요약

ScopedLogger 로깅 인프라(v0.5.11.0) 기반으로 서비스 기동·운영 시 발생하는 로그 패턴의 정상/비정상 판별 가이드를 작성하고, 에이전트 지침을 CLAUDE.md에 연동했다.

## 산출물

| 항목 | 변경 |
|------|------|
| `docs/apex_core/log_patterns_guide.md` | **신규** — 6섹션 (아키텍처 개요, 태그 레퍼런스, 정상 패턴, 비정상 패턴, 트러블슈팅, 컴포넌트 맵) |
| `CLAUDE.md` | 포인터 테이블 1행, 에이전트 지침 1항, 머지 전 갱신 목록 1항 추가 |
| `docs/apex_core/plans/2026-03-24-log-patterns-guide-design.md` | 설계 스펙 |

## 수치

- 컴포넌트 맵: 44개 (apex_core 17 + apex_shared 12 + 서비스 15)
- 비정상 패턴: warn 81건, error 58건, critical 1건 (총 140건)
- 트러블슈팅 체크리스트: 6개 증상 카테고리
