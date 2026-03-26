# 시크릿 관리 강화 구현 계획 — SecureString + ESO/AWS SM

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** password 필드의 메모리 안전성 확보(SecureString) + K8s 외부 Secret Manager 연동(ESO/AWS SM) 인프라 구축

**Architecture:** `SecureString` move-only 래퍼를 `apex_shared`에 추가하고, Kafka/Redis/PG/Gateway/서비스 config의 password 필드를 마이그레이션. Helm umbrella chart에 ESO SecretStore/ExternalSecret 템플릿을 추가하되, 로컬은 `enabled: false`로 무영향.

**Tech Stack:** C++23, Google Test, Helm v2, External Secrets Operator v1beta1, AWS Secrets Manager

**설계 문서:** `docs/apex_common/plans/20260326_182655_secret_management_design.md`

---

## 파일 구조

### 신규 생성

| 파일 | 역할 |
|------|------|
| `apex_shared/lib/include/apex/shared/secure_string.hpp` | SecureString 클래스 헤더 |
| `apex_shared/lib/src/secure_string.cpp` | SecureString 구현 (제로화) |
| `apex_shared/tests/unit/test_secure_string.cpp` | SecureString 단위 테스트 |
| `apex_infra/k8s/apex-pipeline/templates/external-secret-store.yaml` | ESO SecretStore CR |
| `apex_infra/k8s/apex-pipeline/templates/external-secrets.yaml` | ESO ExternalSecret CR |

### 수정 대상

| 파일 | 변경 내용 |
|------|-----------|
| `apex_shared/CMakeLists.txt:37-39` | `secure_string.cpp` 소스 추가 |
| `apex_shared/tests/unit/CMakeLists.txt` | `test_secure_string` 테스트 타겟 추가 |
| `apex_shared/lib/adapters/kafka/include/.../kafka_config.hpp:24` | `sasl_password` → `SecureString` |
| `apex_shared/lib/adapters/redis/include/.../redis_config.hpp:18` | `password` → `SecureString` |
| `apex_shared/lib/adapters/pg/include/.../pg_config.hpp:17` | `connection_string` → `SecureString` |
| `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:37,287,302,375` | `.password` 접근 API 맞춤 |
| `apex_shared/lib/adapters/pg/src/pg_pool.cpp:190,216,246` | `.connection_string` 접근 API 맞춤 |
| `apex_shared/lib/adapters/pg/src/pg_adapter.cpp:28,46` | `.connection_string` 접근 API 맞춤 |
| `apex_services/gateway/include/.../gateway_config.hpp:99-113` | Redis password 3개 → `SecureString` |
| `apex_services/gateway/src/gateway_config_parser.cpp:93,101,109` | `SecureString(expand_env(...))` 래핑 |
| `apex_services/auth-svc/src/main.cpp:69,76-77` | password 파싱을 `SecureString`으로 |
| `apex_services/chat-svc/src/main.cpp:89,98,104-105` | password 파싱을 `SecureString`으로 |
| `apex_infra/k8s/apex-pipeline/values.yaml` | `externalSecrets.enabled: false` 추가 |
| `apex_infra/k8s/apex-pipeline/values-prod.yaml` | ESO 프로덕션 설정 추가 |

---

## Task 1: SecureString 클래스 — 테스트 작성

**Files:**
- Create: `apex_shared/tests/unit/test_secure_string.cpp`

- [ ] **Step 1: 테스트 파일 생성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <apex/shared/secure_string.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

using apex::shared::SecureString;

TEST(SecureStringTest, DefaultConstructedIsEmpty)
{
    SecureString s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_STREQ(s.c_str(), "");
}

TEST(SecureStringTest, ConstructFromStringView)
{
    SecureString s(std::string_view{"hunter2"});
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size(), 7u);
    EXPECT_STREQ(s.c_str(), "hunter2");
    EXPECT_EQ(s.view(), "hunter2");
}

TEST(SecureStringTest, ConstructFromRvalueString)
{
    std::string orig = "secret_password";
    SecureString s(std::move(orig));
    EXPECT_STREQ(s.c_str(), "secret_password");
    EXPECT_EQ(s.size(), 15u);
}

TEST(SecureStringTest, MoveConstruction)
{
    SecureString a(std::string_view{"moveme"});
    SecureString b(std::move(a));

    EXPECT_STREQ(b.c_str(), "moveme");
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(SecureStringTest, MoveAssignment)
{
    SecureString a(std::string_view{"first"});
    SecureString b(std::string_view{"second"});
    b = std::move(a);

    EXPECT_STREQ(b.c_str(), "first");
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(SecureStringTest, CopyIsDeleted)
{
    EXPECT_FALSE(std::is_copy_constructible_v<SecureString>);
    EXPECT_FALSE(std::is_copy_assignable_v<SecureString>);
}

TEST(SecureStringTest, ZeroizesOnDestruction)
{
    // Allocate a string long enough to bypass SSO (> 15 chars typically)
    constexpr std::string_view kSecret = "a_long_secret_that_bypasses_sso_buffer_1234567890";
    const char* raw_ptr = nullptr;

    {
        SecureString s(kSecret);
        raw_ptr = s.c_str();
        // Verify the secret is there before destruction
        ASSERT_EQ(std::memcmp(raw_ptr, kSecret.data(), kSecret.size()), 0);
    }
    // After destruction, the memory should be zeroed.
    // NOTE: Accessing freed memory is technically UB, but this is a best-effort
    // verification for testing purposes. ASAN/MSAN may flag this.
    // We check that at least the first byte is not the original value.
    // This test may be skipped under sanitizers.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_MEMORY__)
    EXPECT_NE(raw_ptr[0], kSecret[0]);
#endif
}

TEST(SecureStringTest, SelfMoveAssignment)
{
    SecureString s(std::string_view{"self"});
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
    s = std::move(s); // NOLINT(bugprone-use-after-move)
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    // Should not crash — either preserved or empty is acceptable
    // Just verify it doesn't segfault
    (void)s.empty();
}
```

- [ ] **Step 2: 커밋 (테스트만, 아직 컴파일 안 됨)**

```bash
git add apex_shared/tests/unit/test_secure_string.cpp
git commit -m "test(shared): SecureString 단위 테스트 추가 (BACKLOG-135)"
```

---

## Task 2: SecureString 클래스 — 구현

**Files:**
- Create: `apex_shared/lib/include/apex/shared/secure_string.hpp`
- Create: `apex_shared/lib/src/secure_string.cpp`
- Modify: `apex_shared/CMakeLists.txt:37-39`
- Modify: `apex_shared/tests/unit/CMakeLists.txt`

- [ ] **Step 1: 헤더 파일 생성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace apex::shared {

/// Move-only string wrapper that zeroes memory on destruction.
/// Use for password, secret key, and other sensitive string fields.
class SecureString
{
  public:
    SecureString() = default;
    ~SecureString();

    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    SecureString(const SecureString&)            = delete;
    SecureString& operator=(const SecureString&) = delete;

    explicit SecureString(std::string_view sv);
    explicit SecureString(std::string&& s) noexcept;

    [[nodiscard]] const char*      c_str() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool             empty() const noexcept;
    [[nodiscard]] std::size_t      size() const noexcept;

  private:
    std::string data_;
    void        clear() noexcept;
};

} // namespace apex::shared
```

- [ ] **Step 2: 구현 파일 생성**

```cpp
// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
#include <apex/shared/secure_string.hpp>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstring> // explicit_bzero
#endif

namespace apex::shared {

void SecureString::clear() noexcept
{
    if (!data_.empty())
    {
        auto* ptr  = data_.data();
        auto  len  = data_.size();
#ifdef _WIN32
        SecureZeroMemory(ptr, len);
#else
        explicit_bzero(ptr, len);
#endif
        data_.clear();
        data_.shrink_to_fit();
    }
}

SecureString::~SecureString() { clear(); }

SecureString::SecureString(std::string_view sv) : data_(sv) {}

SecureString::SecureString(std::string&& s) noexcept : data_(std::move(s)) {}

SecureString::SecureString(SecureString&& other) noexcept : data_(std::move(other.data_)) {}

SecureString& SecureString::operator=(SecureString&& other) noexcept
{
    if (this != &other)
    {
        clear();
        data_ = std::move(other.data_);
    }
    return *this;
}

const char*      SecureString::c_str() const noexcept { return data_.c_str(); }
std::string_view SecureString::view() const noexcept { return data_; }
bool             SecureString::empty() const noexcept { return data_.empty(); }
std::size_t      SecureString::size() const noexcept { return data_.size(); }

} // namespace apex::shared
```

- [ ] **Step 3: `apex_shared/CMakeLists.txt` 소스 등록**

`lib/src/config_utils.cpp` 아래에 `lib/src/secure_string.cpp` 추가:

```cmake
add_library(apex_shared STATIC
    lib/src/config_utils.cpp
    lib/src/secure_string.cpp
)
```

- [ ] **Step 4: `apex_shared/tests/unit/CMakeLists.txt` 테스트 타겟 등록**

기존 `test_config_utils` 블록과 동일 패턴으로 추가:

```cmake
add_executable(test_secure_string test_secure_string.cpp)
target_link_libraries(test_secure_string
    PRIVATE
        apex::shared
        GTest::gtest_main
)
apex_set_warnings(test_secure_string)
add_test(NAME test_secure_string COMMAND test_secure_string)
set_tests_properties(test_secure_string PROPERTIES TIMEOUT 30)
```

- [ ] **Step 5: 빌드 + 테스트 실행**

Run: `apex-agent queue build debug`
Expected: 빌드 성공, `test_secure_string` 전체 통과

- [ ] **Step 6: 커밋**

```bash
git add apex_shared/lib/include/apex/shared/secure_string.hpp \
        apex_shared/lib/src/secure_string.cpp \
        apex_shared/CMakeLists.txt \
        apex_shared/tests/unit/CMakeLists.txt
git commit -m "feat(shared): SecureString 클래스 구현 — 소멸 시 메모리 제로화 (BACKLOG-135)"
```

---

## Task 3: KafkaSecurityConfig 마이그레이션

**Files:**
- Modify: `apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp:24`
- Modify: `apex_shared/lib/adapters/kafka/src/kafka_consumer.cpp:106-108`
- Modify: `apex_shared/lib/adapters/kafka/src/kafka_producer.cpp:93-95`

- [ ] **Step 1: `kafka_config.hpp` — `sasl_password` 타입 변경**

```cpp
// 변경 전 (line 24):
    std::string sasl_password;

// 변경 후:
    SecureString sasl_password;
```

헤더 상단에 include 추가:
```cpp
#include <apex/shared/secure_string.hpp>
```

- [ ] **Step 2: Kafka consumer/producer — 변경 불필요 확인**

`.empty()`, `.c_str()` 호출은 SecureString이 동일 API를 제공하므로 변경 불필요:
```cpp
// kafka_consumer.cpp:106-108 — 그대로 동작
if (!sec.sasl_password.empty())
    if (!sec_set("sasl.password", sec.sasl_password.c_str()))
        return std::unexpected(apex::core::ErrorCode::AdapterError);
```

Producer(93-95)도 동일.

- [ ] **Step 3: Kafka config 파싱 확인**

Kafka config는 TOML에서 직접 파싱하는 코드가 서비스별 `main.cpp`에 있음. Task 6, 7에서 함께 처리.

- [ ] **Step 4: 빌드 확인**

Run: `apex-agent queue build debug`
Expected: 컴파일 성공 (Kafka 파싱 코드는 서비스 빌드에서 확인)

- [ ] **Step 5: 커밋**

```bash
git add apex_shared/lib/adapters/kafka/include/apex/shared/adapters/kafka/kafka_config.hpp
git commit -m "refactor(shared): KafkaSecurityConfig::sasl_password → SecureString (BACKLOG-135)"
```

---

## Task 4: RedisConfig 마이그레이션

**Files:**
- Modify: `apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_config.hpp:18`
- Modify: `apex_shared/lib/adapters/redis/src/redis_multiplexer.cpp:37,287,302,375`

- [ ] **Step 1: `redis_config.hpp` — `password` 타입 변경**

```cpp
// 변경 전 (line 18):
    std::string password; ///< AUTH 비밀번호 (빈 문자열 = 미사용)

// 변경 후:
    SecureString password; ///< AUTH 비밀번호 (빈 문자열 = 미사용)
```

헤더 상단에 include 추가:
```cpp
#include <apex/shared/secure_string.hpp>
```

- [ ] **Step 2: `redis_multiplexer.cpp` — 접근 코드 확인**

기존 사용:
```cpp
// line 37: config_.password.empty() → SecureString::empty() — 호환
// line 287: config_.password.empty() → 호환
// line 302: config_.password.c_str() → SecureString::c_str() — 호환
// line 375: config_.password.empty() ? "" : " (authenticated)" → 호환
```

**모두 `.empty()`, `.c_str()` 호출이므로 변경 불필요.**

- [ ] **Step 3: 빌드 확인**

Run: `apex-agent queue build debug`
Expected: 컴파일 성공

- [ ] **Step 4: 커밋**

```bash
git add apex_shared/lib/adapters/redis/include/apex/shared/adapters/redis/redis_config.hpp
git commit -m "refactor(shared): RedisConfig::password → SecureString (BACKLOG-135)"
```

---

## Task 5: PgAdapterConfig 마이그레이션

**Files:**
- Modify: `apex_shared/lib/adapters/pg/include/apex/shared/adapters/pg/pg_config.hpp:17`
- Modify: `apex_shared/lib/adapters/pg/src/pg_pool.cpp:190,216,246`
- Modify: `apex_shared/lib/adapters/pg/src/pg_adapter.cpp:28,46`

- [ ] **Step 1: `pg_config.hpp` — `connection_string` 타입 변경**

```cpp
// 변경 전 (line 17):
    std::string connection_string = "host=localhost port=6432 dbname=apex user=apex";

// 변경 후:
    SecureString connection_string{std::string_view{"host=localhost port=6432 dbname=apex user=apex"}};
```

헤더 상단에 include 추가:
```cpp
#include <apex/shared/secure_string.hpp>
```

- [ ] **Step 2: `pg_pool.cpp` — `connection_string` 접근부 수정**

```cpp
// line 190: connect_async에 std::string이 필요한 경우
auto result = co_await conn->connect_async(std::string{config_.connection_string.view()});

// line 216: 동일 패턴
auto connect_result = co_await conn->connect_async(std::string{config_.connection_string.view()});

// line 246: getter — 반환 타입을 string_view로 변경하거나 view() 사용
return config_.connection_string.view();
```

`connect_async`가 `std::string_view` 또는 `const std::string&`을 받는지 확인하고 최소 변환 적용. `const char*`를 받으면 `.c_str()` 사용.

- [ ] **Step 3: `pg_adapter.cpp` — `connection_string` 접근부 수정**

```cpp
// line 28: empty 체크 — 호환
if (config_.connection_string.empty())

// line 46: 마스킹용 복사 — view()에서 std::string 생성
auto masked = std::string{config_.connection_string.view()};
```

- [ ] **Step 4: 빌드 확인**

Run: `apex-agent queue build debug`
Expected: 컴파일 성공

- [ ] **Step 5: 커밋**

```bash
git add apex_shared/lib/adapters/pg/include/apex/shared/adapters/pg/pg_config.hpp \
        apex_shared/lib/adapters/pg/src/pg_pool.cpp \
        apex_shared/lib/adapters/pg/src/pg_adapter.cpp
git commit -m "refactor(shared): PgAdapterConfig::connection_string → SecureString (BACKLOG-135)"
```

---

## Task 6: GatewayConfig + 서비스 Config 마이그레이션

**Files:**
- Modify: `apex_services/gateway/include/apex/gateway/gateway_config.hpp:99-113`
- Modify: `apex_services/gateway/src/gateway_config_parser.cpp:93,101,109`
- Modify: `apex_services/auth-svc/src/main.cpp:69,76-77`
- Modify: `apex_services/chat-svc/src/main.cpp:89,98,104-105`

- [ ] **Step 1: `gateway_config.hpp` — Redis password 3개 타입 변경**

```cpp
// 변경 전:
    std::string redis_pubsub_password;
    std::string redis_auth_password;
    std::string redis_ratelimit_password;

// 변경 후:
    apex::shared::SecureString redis_pubsub_password;
    apex::shared::SecureString redis_auth_password;
    apex::shared::SecureString redis_ratelimit_password;
```

헤더에 include 추가:
```cpp
#include <apex/shared/secure_string.hpp>
```

- [ ] **Step 2: `gateway_config_parser.cpp` — `SecureString` 래핑**

```cpp
// 변경 전 (line 93):
cfg.redis_pubsub_password = apex::shared::expand_env(rpub["password"].value_or(std::string{}));

// 변경 후:
cfg.redis_pubsub_password = apex::shared::SecureString(apex::shared::expand_env(rpub["password"].value_or(std::string{})));

// line 101, 109도 동일 패턴 적용
```

- [ ] **Step 3: Gateway Redis 어댑터 생성부 확인**

Gateway에서 `RedisConfig`를 만들 때 password를 이동시키는 코드 확인. `std::string` 대입이었으면 `SecureString` 이동으로 변경:

```cpp
// 예시 — 실제 코드 확인 후 조정
redis_config.password = std::move(config_.redis_auth_password);
// 또는 SecureString끼리 이동이면 그대로 동작
```

- [ ] **Step 4: `auth-svc/src/main.cpp` — password 파싱 SecureString 래핑**

```cpp
// 변경 전 (line 69):
cfg.redis.password = apex::shared::expand_env(redis["password"].value_or(std::string{}));

// 변경 후:
cfg.redis.password = apex::shared::SecureString(apex::shared::expand_env(redis["password"].value_or(std::string{})));

// PG connection_string (line 76-77):
cfg.pg.connection_string = apex::shared::SecureString(apex::shared::expand_env(pg["connection_string"].value_or(
    std::string{"host=localhost port=5432 dbname=apex_db user=apex_user password=${PG_AUTH_PASSWORD:-}"})));
```

- [ ] **Step 5: `chat-svc/src/main.cpp` — password 파싱 SecureString 래핑**

```cpp
// redis_data password (line 89):
cfg.redis_data.password = apex::shared::SecureString(apex::shared::expand_env(redis_data["password"].value_or(std::string{})));

// redis_pubsub password (line 98):
cfg.redis_pubsub.password = apex::shared::SecureString(apex::shared::expand_env(redis_pubsub["password"].value_or(std::string{})));

// PG connection_string (line 104-105):
cfg.pg.connection_string = apex::shared::SecureString(apex::shared::expand_env(
    pg["connection_string"].value_or(std::string{"host=localhost port=6432 dbname=apex user=apex"})));
```

- [ ] **Step 6: Kafka security config 파싱 확인**

각 서비스의 main.cpp에서 `KafkaSecurityConfig::sasl_password`를 설정하는 코드도 `SecureString` 래핑 필요. 파싱 코드 확인 후 적용.

- [ ] **Step 7: 빌드 + 전체 테스트**

Run: `apex-agent queue build debug`
Expected: 전체 빌드 성공, 기존 테스트 모두 통과

- [ ] **Step 8: 커밋**

```bash
git add apex_services/gateway/include/apex/gateway/gateway_config.hpp \
        apex_services/gateway/src/gateway_config_parser.cpp \
        apex_services/auth-svc/src/main.cpp \
        apex_services/chat-svc/src/main.cpp
git commit -m "refactor(gateway,auth_svc,chat_svc): password 필드 전체 SecureString 마이그레이션 (BACKLOG-135)"
```

---

## Task 7: ESO Helm 템플릿 — SecretStore + ExternalSecret

**Files:**
- Create: `apex_infra/k8s/apex-pipeline/templates/external-secret-store.yaml`
- Create: `apex_infra/k8s/apex-pipeline/templates/external-secrets.yaml`

- [ ] **Step 1: SecretStore 템플릿 생성**

```yaml
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
{{- if .Values.externalSecrets.enabled }}
apiVersion: external-secrets.io/v1beta1
kind: SecretStore
metadata:
  name: apex-secret-store
  namespace: {{ .Release.Namespace }}
  labels:
    app.kubernetes.io/name: apex-pipeline
    app.kubernetes.io/instance: {{ .Release.Name }}
spec:
  provider:
    aws:
      service: SecretsManager
      region: {{ .Values.externalSecrets.aws.region | quote }}
      auth:
        jwt:
          serviceAccountRef:
            name: {{ .Values.externalSecrets.serviceAccount | quote }}
{{- end }}
```

- [ ] **Step 2: ExternalSecret 템플릿 생성**

```yaml
# Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.
{{- if .Values.externalSecrets.enabled }}
{{- range $name, $svc := .Values.externalSecrets.secrets }}
---
apiVersion: external-secrets.io/v1beta1
kind: ExternalSecret
metadata:
  name: {{ $name }}-external
  namespace: {{ $.Release.Namespace }}
  labels:
    app.kubernetes.io/name: apex-pipeline
    app.kubernetes.io/instance: {{ $.Release.Name }}
spec:
  refreshInterval: "0"
  secretStoreRef:
    name: apex-secret-store
    kind: SecretStore
  target:
    name: {{ $svc.targetSecret }}
    creationPolicy: Owner
  data:
    {{- range $svc.keys }}
    - secretKey: {{ .envVar }}
      remoteRef:
        key: {{ $.Values.externalSecrets.aws.secretPrefix }}/{{ $name }}
        property: {{ .property }}
    {{- end }}
{{- end }}
{{- end }}
```

- [ ] **Step 3: 커밋**

```bash
git add apex_infra/k8s/apex-pipeline/templates/external-secret-store.yaml \
        apex_infra/k8s/apex-pipeline/templates/external-secrets.yaml
git commit -m "feat(infra): ESO SecretStore + ExternalSecret 템플릿 추가 (BACKLOG-198)"
```

---

## Task 8: Helm values 업데이트

**Files:**
- Modify: `apex_infra/k8s/apex-pipeline/values.yaml`
- Modify: `apex_infra/k8s/apex-pipeline/values-prod.yaml`

- [ ] **Step 1: `values.yaml` — externalSecrets 비활성화 기본값 추가**

파일 끝에 추가:

```yaml
# --- External Secrets Operator ---
# 로컬 환경에서는 ESO 비활성화 — 기존 secrets.data로 K8s Secret 직접 생성
externalSecrets:
  enabled: false
```

- [ ] **Step 2: `values-prod.yaml` — ESO 프로덕션 설정 추가**

파일 끝에 추가:

```yaml
# --- External Secrets Operator ---
externalSecrets:
  enabled: true
  serviceAccount: apex-secrets-sa
  aws:
    region: ap-northeast-2
    secretPrefix: apex
  secrets:
    gateway:
      targetSecret: apex-gateway-secrets
      keys:
        - envVar: REDIS_AUTH_PASSWORD
          property: redis_auth_password
        - envVar: REDIS_RATELIMIT_PASSWORD
          property: redis_ratelimit_password
        - envVar: REDIS_PUBSUB_PASSWORD
          property: redis_pubsub_password
    auth-svc:
      targetSecret: apex-auth-secrets
      keys:
        - envVar: REDIS_AUTH_PASSWORD
          property: redis_password
        - envVar: PG_SERVICE_PASSWORD
          property: pg_password
    chat-svc:
      targetSecret: apex-chat-secrets
      keys:
        - envVar: REDIS_CHAT_PASSWORD
          property: redis_data_password
        - envVar: REDIS_PUBSUB_PASSWORD
          property: redis_pubsub_password
        - envVar: PG_SERVICE_PASSWORD
          property: pg_password
    rsa-keys:
      targetSecret: apex-rsa-keys
      keys:
        - envVar: RSA_PRIVATE_KEY
          property: private_key
        - envVar: RSA_PUBLIC_KEY
          property: public_key
```

- [ ] **Step 3: Helm lint 검증**

Run: `helm lint apex_infra/k8s/apex-pipeline/`
Expected: 통과 (ESO CRD 없어도 `enabled: false`이므로 템플릿 렌더링 안 됨)

Run: `helm template apex-test apex_infra/k8s/apex-pipeline/`
Expected: external-secret 관련 리소스 없음 (enabled=false)

- [ ] **Step 4: 커밋**

```bash
git add apex_infra/k8s/apex-pipeline/values.yaml \
        apex_infra/k8s/apex-pipeline/values-prod.yaml
git commit -m "feat(infra): ESO values 설정 — 로컬 비활성화, 프로덕션 AWS SM (BACKLOG-198)"
```

---

## Task 9: clang-format + 최종 빌드 검증

- [ ] **Step 1: clang-format 실행**

```bash
find apex_core apex_shared apex_services \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) ! -name '*_generated.h' | xargs clang-format -i
```

- [ ] **Step 2: 최종 빌드 + 전체 테스트**

Run: `apex-agent queue build debug`
Expected: 전체 빌드 성공, 전체 테스트 통과 (test_secure_string 포함)

- [ ] **Step 3: 변경사항 커밋 (포맷팅 변경 있는 경우)**

```bash
git add -A
git commit -m "style: clang-format 적용"
```
