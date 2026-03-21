# FSD Backlog Sweep — 작업 완료

- **브랜치**: feature/fsd-backlog-20260322_003257
- **작업 일시**: 2026-03-22

## 해결 항목 (3건)

### BACKLOG-125: init.sql 고아 스키마 정리
- `auth_schema`, `chat_schema`, `match_schema` CREATE 제거
- 실제 서비스는 마이그레이션에서 `auth_svc`, `chat_svc`를 직접 생성

### BACKLOG-124: jwt_blacklist is_valid_jti 입력 검증 테스트
- `is_valid_jti`를 anonymous namespace에서 `detail` namespace로 이동
- 단위 테스트 16건 작성 (빈 문자열, 128자 경계, hex 범위 외, Redis injection, 하이픈 전용 등)
- `test_gateway_jwt_blacklist` CMake 타겟 추가

### BACKLOG-63: docs/CLAUDE.md 백로그 운영 규칙 중복 정리
- 루트 CLAUDE.md에서 중복 규칙 3개 제거 → 포인터로 축소
- docs/CLAUDE.md에 progress 문서 품질 요건 이관
- 단일 권위 출처 역할 분리 명확화

## FSD 분석 태그 (5건)

| # | 사유 |
|---|------|
| #59 | 5종 도구 설계 판단 필요 |
| #19 | 테스트 대상 선정·mock 전략 설계 판단 필요 |
| #102 | 코루틴+Redis mock 인프라 선행 구축 필요 |
| #123 | #102 의존, 단독 자동화 불가 |
| #122 | build cache-from 전략 재설계 필요 |

## 빌드/테스트

- 로컬 빌드: 81/81 통과 (test_cross_core_call flaky는 기존 이슈, 변경 무관)
- 신규 테스트: test_gateway_jwt_blacklist 16케이스 전체 통과
