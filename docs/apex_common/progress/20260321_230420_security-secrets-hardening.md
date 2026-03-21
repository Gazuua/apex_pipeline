# 보안 시크릿 관리 + Blacklist 정책 완료 기록

- **백로그**: #5, #6, #8, #100
- **브랜치**: `feature/security-secrets-hardening`
- **스코프**: infra, gateway, auth-svc, chat-svc
- **일시**: 2026-03-21

---

## 완료 내용

### 인프라 레이어
- Redis 4개 인스턴스에 `--requirepass` 인증 추가 (docker-compose.yml, docker-compose.e2e.yml)
- Redis healthcheck에 `REDISCLI_AUTH` 환경 변수 추가
- PgBouncer 정적 `userlist.txt`(평문 비밀번호) 삭제 → 동적 entrypoint.sh로 md5 해시 생성
- SQL 마이그레이션 `CREATE ROLE` 비밀번호를 쉘 래퍼로 분리 + SQL injection 방어
- `.env.dev`(dev 기본값, VCS 커밋) + `.env.prod.example`(템플릿) 분리
- 기존 `.env.example` 삭제

### 코드 레이어
- `expand_env()` 함수를 `apex_shared`로 추출 (gateway 익명 네임스페이스 → `apex::shared::expand_env`)
- gateway/auth-svc/chat-svc config parser에서 공통 유틸 사용
- TOML 설정 파일에 `${VAR:-default}` 패턴 적용 (Redis password, PG connection_string)
- `blacklist_fail_open` 설정 옵션 추가 (기본값 `false` = fail-close)
- `jwt_blacklist.cpp`: Redis 실패 시 사실 보고 (`std::unexpected(AdapterError)`)
- `gateway_pipeline.cpp`: 설정 기반 fail-open/fail-close 분기
- `GatewayError::BlacklistCheckFailed = 12` 추가

### 테스트
- `test_config_utils.cpp`: expand_env 단위 테스트 11건 (기본 7 + 엣지 4)
- `test_gateway_pipeline.cpp`: blacklist_fail_open 기본값 검증
- `test_config_reloader.cpp`: blacklist_fail_open 파싱 검증

### 설계 원칙
- 개발 편의 최우선 — 로컬/CI/E2E 무설정 동작 유지
- `${VAR:-dev_default}` 패턴 — 환경 변수 미설정 시 dev 기본값 자동 적용

## 빌드/테스트 결과
- MSVC (Windows, Debug): 80/80 테스트 통과
