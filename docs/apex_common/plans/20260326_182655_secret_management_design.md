# 시크릿 관리 강화 — SecureString + ESO/AWS Secrets Manager

- **백로그**: BACKLOG-135, BACKLOG-198
- **브랜치**: `feature/secret-management`
- **스코프**: shared, infra
- **버전**: v0.6.4 시점

---

## 배경

v0.5.10 시크릿 하드닝(PR #131)에서 `expand_env()` 패턴, 로그 마스킹, Docker Compose 환경변수 주입을 완료했다. 그때 명시적으로 v0.6 DEFERRED로 남긴 두 가지:

1. **BACKLOG-135**: `KafkaSecurityConfig::sasl_password` 등 password 필드가 `std::string` 평문으로 프로세스 수명 내내 메모리 상주
2. **BACKLOG-198**: K8s `existingSecret` 인터페이스만 준비됨 — 실제 외부 Secret Manager 연동 없음

---

## 설계 원칙

- **로컬 무영향** — Docker Compose + `.env.dev` + `${VAR:-default}` 기존 패턴 그대로
- **접근법 A (실용적 최소)** — 메모리 제로화까지만, mlock/로테이션은 스코프 외
- **AWS 기본, 추상화 확보** — ESO의 SecretStore/ExternalSecret 분리로 백엔드 교체 가능

---

## 섹션 1: SecureString (BACKLOG-135)

### 1-1. 클래스 설계

```cpp
// apex_shared/lib/include/apex/shared/secure_string.hpp
namespace apex::shared {

class SecureString {
public:
    SecureString() = default;
    ~SecureString();                          // 메모리 제로화

    // copyable — 복사본도 소멸 시 독립 제로화
    // (move-only 설계에서 변경: MSVC aggregate init 호환 + 기존 코드 영향 최소화)
    SecureString(const SecureString& other);
    SecureString& operator=(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    // 생성 — const char*/string_view는 implicit (aggregate init 호환)
    SecureString(const char* s);
    SecureString(std::string_view sv);
    explicit SecureString(std::string&& s);

    // 접근
    [[nodiscard]] const char* c_str() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    // 비교 연산자
    [[nodiscard]] bool operator==(const SecureString& other) const noexcept;
    [[nodiscard]] bool operator==(std::string_view other) const noexcept;
    [[nodiscard]] bool operator==(const char* other) const noexcept;
    [[nodiscard]] bool operator!=(const SecureString& other) const noexcept;
    [[nodiscard]] bool operator!=(std::string_view other) const noexcept;
    [[nodiscard]] bool operator!=(const char* other) const noexcept;

private:
    std::string data_;
    void clear() noexcept;                    // 제로화 + shrink
};

} // namespace apex::shared
```

### 1-2. 제로화 구현

```cpp
void SecureString::clear() noexcept {
    if (!data_.empty()) {
#ifdef _WIN32
        SecureZeroMemory(data_.data(), data_.size());
#else
        explicit_bzero(data_.data(), data_.size());
#endif
        data_.clear();
        data_.shrink_to_fit();    // SSO 버퍼도 해제 유도
    }
}

SecureString::~SecureString() { clear(); }
```

- `explicit_bzero`/`SecureZeroMemory`: 컴파일러가 dead store로 최적화하지 못하도록 보장
- `shrink_to_fit()`: SSO(Small String Optimization) 범위 내 문자열도 내부 버퍼 해제 유도
- **SSO 한계**: 15자 이하 문자열은 std::string 내부 SSO 버퍼에 저장되며, `shrink_to_fit()` 후에도 잔존할 수 있음. 이는 std::string 구현의 한계이며, 프로덕션 password는 일반적으로 SSO 임계를 초과

### 1-3. 적용 대상

| 구조체 | 필드 | 파일 |
|--------|------|------|
| `KafkaSecurityConfig` | `sasl_password` | `kafka_config.hpp` |
| Gateway `GatewayConfig` | `redis_auth_password`, `redis_ratelimit_password`, `redis_pubsub_password` | `gateway_config.hpp` |
| Auth 서비스 config | `redis_password`, PG connection_string | `main.cpp` 내 config 구조체 |
| Chat 서비스 config | `redis_data_password`, `redis_pubsub_password`, PG connection_string | `main.cpp` 내 config 구조체 |

PG connection_string은 password만 분리하면 파싱 복잡도가 올라가므로 전체 string을 SecureString으로 처리.

### 1-4. 호출부 영향

- `sec.sasl_password.c_str()` → 동일 API, 변경 없음
- `sec.sasl_password.empty()` → 동일 API, 변경 없음
- `expand_env()` 반환값을 `SecureString`으로 래핑: `SecureString(expand_env(...))`
- 복사 허용 → 기존 코드 변경 최소화. 각 복사본은 독립적으로 소멸 시 제로화

### 1-5. 테스트

- `test_secure_string.cpp` 신규: 생성/이동/제로화 검증, empty 체크, move semantics
- 제로화 검증: 소멸 후 원본 버퍼 주소의 내용이 0인지 확인 (UB 영역이지만 테스트 목적으로 허용)
- 기존 Kafka/Redis 테스트: 타입 변경에 따른 컴파일 확인

---

## 섹션 2: ESO + AWS Secrets Manager (BACKLOG-198)

### 2-1. Helm 구조

```
apex_infra/k8s/apex-pipeline/
├── templates/
│   ├── external-secret-store.yaml    ← 신규
│   └── external-secrets.yaml         ← 신규
├── values.yaml                       ← externalSecrets 섹션 추가
└── values-prod.yaml                  ← AWS 설정 추가
```

기존 서비스 차트(`charts/`)와 apex-common 템플릿은 변경 없음.

### 2-2. SecretStore CR

```yaml
{{- if .Values.externalSecrets.enabled }}
apiVersion: external-secrets.io/v1beta1
kind: SecretStore
metadata:
  name: apex-secret-store
  labels:
    {{- include "apex-pipeline.labels" . | nindent 4 }}
spec:
  provider:
    aws:
      service: SecretsManager
      region: {{ .Values.externalSecrets.aws.region }}
      auth:
        jwt:
          serviceAccountRef:
            name: {{ .Values.externalSecrets.serviceAccount }}
{{- end }}
```

- IRSA (IAM Roles for Service Accounts)로 인증 — Pod에 AWS 크레덴셜 불필요
- 백엔드 교체 시 `spec.provider` 섹션만 변경

### 2-3. ExternalSecret CR (서비스별)

```yaml
{{- if .Values.externalSecrets.enabled }}
{{- range $name, $svc := .Values.externalSecrets.secrets }}
---
apiVersion: external-secrets.io/v1beta1
kind: ExternalSecret
metadata:
  name: {{ $name }}-external
spec:
  refreshInterval: "0"
  secretStoreRef:
    name: apex-secret-store
  target:
    name: {{ $svc.targetSecret }}    # 기존 existingSecret 이름
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

- `target.name`이 기존 `existingSecret`과 일치 → Helm 템플릿, 서비스 코드 변경 없음
- `refreshInterval: "0"` — 로테이션 미지원 (접근법 A)

### 2-4. values.yaml (로컬)

```yaml
externalSecrets:
  enabled: false    # 로컬에서는 ESO 비활성화
```

로컬은 기존 `secrets.data`로 K8s Secret 직접 생성 → 변경 없음.

### 2-5. values-prod.yaml (프로덕션)

```yaml
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

### 2-6. AWS Secrets Manager 시크릿 구조 (운영 가이드)

| AWS SM 경로 | 키 | 대응 K8s Secret |
|-------------|-----|----------------|
| `apex/gateway` | `redis_auth_password`, `redis_ratelimit_password`, `redis_pubsub_password` | `apex-gateway-secrets` |
| `apex/auth-svc` | `redis_password`, `pg_password` | `apex-auth-secrets` |
| `apex/chat-svc` | `redis_data_password`, `redis_pubsub_password`, `pg_password` | `apex-chat-secrets` |
| `apex/rsa-keys` | `private_key`, `public_key` | `apex-rsa-keys` |

실제 AWS SM 시크릿 생성은 운영팀 몫. ESO CRD와 이 매핑 가이드만 제공.

### 2-7. 전제 조건

ESO는 클러스터에 사전 설치 필요:
```bash
helm repo add external-secrets https://charts.external-secrets.io
helm install external-secrets external-secrets/external-secrets -n external-secrets --create-namespace
```

이 설치는 클러스터 관리자 영역이므로 apex-pipeline chart 스코프 밖.

---

## 스코프 외

- mlock / VirtualLock (swap 방지) — K8s 노드 레벨 swap off가 표준
- 시크릿 로테이션 (`refreshInterval` > 0 + 앱 hot-reload) — 인프라 운영 후 별도 작업
- Redis ACL, PgBouncer SCRAM-SHA-256 — TLS 도입과 함께
- ESO 클러스터 설치 자동화 — 클러스터 부트스트랩 스코프
