# ESO SecretStore Per-Service RBAC (BACKLOG-252)

- **브랜치**: `feature/eso-per-service-rbac`
- **PR**: #209

## 작업 결과

단일 SecretStore + ServiceAccount 구조에서 **서비스별 독립 SecretStore + ServiceAccount**로 전환 완료.

### 변경 사항

| 파일 | 변경 |
|------|------|
| `templates/external-secret-store.yaml` | secrets map loop → per-service SA + SecretStore 생성 |
| `templates/external-secrets.yaml` | secretStoreRef 동적 참조 (`{{ $name }}-secret-store`) |
| `values-prod.yaml` | 글로벌 `serviceAccount` 제거, 서비스별 `iamRoleArn` 추가 |

### 생성되는 리소스 (prod)

| ServiceAccount | SecretStore | ExternalSecret |
|---------------|-------------|----------------|
| gateway-secrets-sa | gateway-secret-store | gateway-external |
| auth-svc-secrets-sa | auth-svc-secret-store | auth-svc-external |
| chat-svc-secrets-sa | chat-svc-secret-store | chat-svc-external |
| rsa-keys-secrets-sa | rsa-keys-secret-store | rsa-keys-external |

### AWS IAM 요구사항 (배포 시 운영팀)

- 서비스별 IAM role 생성 + EKS OIDC provider IRSA 매핑
- 각 role의 SM 접근을 `apex/<service>/*` prefix로 제한
- `values-prod.yaml`의 `iamRoleArn`에 실제 ARN 설정
