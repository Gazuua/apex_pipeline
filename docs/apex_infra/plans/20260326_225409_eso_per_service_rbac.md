# ESO SecretStore Per-Service RBAC 설계 (BACKLOG-252)

## 문제

현재 단일 SecretStore(`apex-secret-store`) + 단일 ServiceAccount(`apex-secrets-sa`)로
모든 ExternalSecret이 AWS SM 전체에 접근 가능.
한 서비스 Pod 침해 시 다른 서비스의 시크릿까지 노출 (blast radius 문제).

## 선택: Option B — Per-Service SecretStore

| 옵션 | 설명 | 판정 |
|------|------|------|
| A) SA 분리 + IAM prefix | SA만 분리, SecretStore 공유 | SecretStore가 단일이면 SA 분리 효과 제한 |
| **B) Per-Service SecretStore** | 서비스별 SA + SecretStore + IAM role | **채택** — 완전 격리, template loop로 확장 자동화 |
| C) ClusterSecretStore + NS 분리 | 네임스페이스 기반 격리 | 현재 단일 NS 구조에서 대규모 변경 필요 |

## 변경 범위

### 1. `templates/external-secret-store.yaml`
- 기존: 단일 SecretStore 1개 생성
- 변경: `externalSecrets.secrets` map loop → 서비스별 ServiceAccount + SecretStore 생성
- SA 이름 패턴: `{{ $name }}-secrets-sa`
- SecretStore 이름 패턴: `{{ $name }}-secret-store`
- IAM role annotation: `eks.amazonaws.com/role-arn` (values에서 주입)

### 2. `templates/external-secrets.yaml`
- 기존: `secretStoreRef.name: apex-secret-store` (hardcoded)
- 변경: `secretStoreRef.name: {{ $name }}-secret-store` (동적 참조)

### 3. `values-prod.yaml`
- 제거: `externalSecrets.serviceAccount` (글로벌 단일 SA)
- 추가: 각 secret 엔트리에 `iamRoleArn` 필드 (배포 시 운영팀이 ARN 설정)

### 4. AWS IAM 요구사항 (코드 외)
- 서비스별 IAM role 생성 (`apex-gateway-secrets-role`, `apex-auth-secrets-role` 등)
- 각 role의 SM 접근을 `apex/<service>/*` prefix로 제한
- EKS OIDC provider와 SA 매핑 (IRSA)

## 결과 구조

```
Before:
  apex-secrets-sa → IAM-role-all → apex/*

After:
  gateway-secrets-sa    → IAM-gateway    → apex/gateway/*
  auth-svc-secrets-sa   → IAM-auth-svc   → apex/auth-svc/*
  chat-svc-secrets-sa   → IAM-chat-svc   → apex/chat-svc/*
  rsa-keys-secrets-sa   → IAM-rsa-keys   → apex/rsa-keys/*
```
