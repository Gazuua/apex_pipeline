# 시크릿 관리 강화 완료 기록

- **백로그**: BACKLOG-135, BACKLOG-198
- **브랜치**: `feature/secret-management`
- **스코프**: shared, infra, gateway, auth-svc, chat-svc
- **일시**: 2026-03-26

---

## 완료 내용

### SecureString 클래스 (BACKLOG-135)

- `apex::shared::SecureString` — 소멸 시 `SecureZeroMemory`(Windows) / `explicit_bzero`(Linux)로 메모리 제로화
- Copyable (각 복사본 독립 제로화), implicit `const char*`/`string_view` 생성자 (aggregate init 호환)
- `operator==`/`!=` 지원 (`SecureString`, `string_view`, `const char*`)
- 테스트: 8건 (생성, 이동, 복사, 제로화, self-move)

### Password 필드 마이그레이션

| 구조체 | 필드 | 파일 |
|--------|------|------|
| `KafkaSecurityConfig` | `sasl_password` | `kafka_config.hpp` |
| `RedisConfig` | `password` | `redis_config.hpp` |
| `PgAdapterConfig` | `connection_string` | `pg_config.hpp` |
| `GatewayConfig` | `redis_pubsub_password`, `redis_auth_password`, `redis_ratelimit_password` | `gateway_config.hpp` |
| `PubSubListener::Config` | `password` | `pubsub_listener.hpp` |

### 서비스 Config Parser

- Gateway, Auth, Chat 서비스의 `expand_env()` 결과를 `SecureString`으로 래핑
- `PgPool::connection_string()` 반환 타입 `const std::string&` → `std::string_view`

### ESO + AWS Secrets Manager (BACKLOG-198)

- `templates/external-secret-store.yaml` — SecretStore CR (AWS SM + IRSA)
- `templates/external-secrets.yaml` — ExternalSecret CR (서비스별 시크릿 매핑)
- `values.yaml`: `externalSecrets.enabled: false` (로컬 무영향)
- `values-prod.yaml`: AWS SM 프로덕션 설정 (ap-northeast-2, 서비스별 4개 시크릿)

### CMake 의존성 정리

- `apex_shared_adapters_common` → `apex::shared` 의존성 추가 (include 경로 전파)
- Gateway 테스트 타겟 8개에 `apex_shared` 의존성 추가

## 빌드/테스트 결과

- MSVC (Windows, Debug): 97/97 통과
- GCC (Linux, Debug): 92/92 통과
- GCC (Linux, Release): 92/92 통과
- Clang ASAN: 92/92 통과
- Clang UBSAN: 92/92 통과
- Clang TSAN: 92/92 통과
- format-check: 통과
