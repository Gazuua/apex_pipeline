# apex-pipeline Helm Chart -- 에이전트 + 개발자 가이드

## 1. 차트 수정 규칙

### 수정 가능 파일과 용도

| 파일 | 수정 시점 | 주의사항 |
|------|-----------|----------|
| `Chart.yaml` | sub-chart/의존성 추가, 버전 범프 | `version`과 `appVersion` 동시 갱신 |
| `values.yaml` | 로컬 기본값 변경 | 시크릿 평문은 개발용만 허용 |
| `values-prod.yaml` | 프로덕션 설정 변경 | 평문 시크릿 절대 금지 (existingSecret만) |
| `charts/<service>/values.yaml` | 서비스별 기본값 변경 | umbrella values에서 오버라이드 가능한 값만 |
| `charts/<service>/templates/*.yaml` | 서비스 전용 리소스 추가/변경 | 공통 로직은 apex-common에 넣을지 먼저 판단 |
| `charts/apex-common/templates/_helpers.tpl` | 공통 named template 변경 | 모든 서비스에 영향 -- 변경 후 전체 lint 필수 |
| `templates/` | umbrella 레벨 리소스 (namespace, 공통 secret) | 서비스별 리소스를 여기에 넣지 않기 |
| `scripts/` | 로컬 셋업/정리 스크립트 | 2-release 순서 유지 |

### 수정 흐름

1. 변경 대상 파일 수정
2. `helm dependency update .` (Chart.yaml 변경 시)
3. `helm lint .` -- 구문 오류 검사
4. `helm template .` -- 렌더링 결과 확인
5. 커밋

### sub-chart values.yaml vs umbrella values.yaml

- `charts/<service>/values.yaml`: 서비스의 **기본값**을 정의. 모든 환경에서 공통으로 쓰이는 값
- `values.yaml` (umbrella): **로컬 환경 오버라이드**. Helm이 `<service-name>.<key>` 형태로 sub-chart values를 오버라이드
- `values-prod.yaml`: **프로덕션 오버라이드**. `-f values.yaml -f values-prod.yaml` 순서로 적용

예시: Gateway의 replicas를 프로덕션에서 3으로:

```yaml
# values-prod.yaml
gateway:
  replicaCount: 3
```

---

## 2. values 스키마 규칙

### 새 값 추가 시 패턴

모든 서비스 sub-chart의 values.yaml은 `apex-common` library chart가 기대하는 스키마를 따라야 한다. 스키마 전문은 `charts/apex-common/README.md`에 정의되어 있다.

### 필수 키

```yaml
replicaCount: <int>
image:
  repository: <string>
  tag: <string>
  pullPolicy: <string>
service:
  type: ClusterIP
  ports: <list>
config:
  fileName: <string>
  mountPath: <string>
  content: <multiline string>
probes:
  startup: <probe spec>
  liveness: <probe spec>
  readiness: <probe spec>
resources:
  requests: { cpu: <string>, memory: <string> }
  limits: { cpu: <string>, memory: <string> }
```

### 선택 키

```yaml
secrets:
  existingSecret: ""           # 비어있으면 secrets.data로 자동 생성
  data: {}                     # key-value (Secret stringData로 변환)
extraVolumes: []
extraVolumeMounts: []
extraEnv: []
serviceAccount:
  create: false
  name: ""
  annotations: {}
serviceMonitor:
  enabled: true
  port: metrics
  path: /metrics
  interval: 15s
nameOverride: ""
fullnameOverride: ""
```

### 새 값 추가 규칙

1. **공통 값은 apex-common 스키마에 추가** -- `_helpers.tpl`의 named template이 참조하는 값
2. **서비스 전용 값은 해당 sub-chart values.yaml에 추가** -- 예: gateway의 `ingress`, `hpa`, `pdb`
3. **환경별 오버라이드가 필요한 값은 umbrella values.yaml과 values-prod.yaml 양쪽에 기재**
4. **새 키 이름은 camelCase** -- `replicaCount`, `pullPolicy`, `existingSecret`
5. **boolean 토글은 `.enabled` 패턴** -- `hpa.enabled`, `pdb.enabled`
6. **기본값은 항상 로컬 환경에 맞게 설정** -- 프로덕션은 values-prod.yaml에서 오버라이드

---

## 3. 새 sub-chart 추가 절차

### 체크리스트

- [ ] `charts/<service-name>/` 디렉토리 생성
- [ ] `Chart.yaml` 작성 (apex-common 의존성 포함)
- [ ] `values.yaml` 작성 (apex-common 스키마 준수)
- [ ] `templates/deployment.yaml` -- `{{- include "apex-common.deployment" . }}`
- [ ] `templates/service.yaml` -- `{{- include "apex-common.service" . }}`
- [ ] `templates/configmap.yaml` -- `{{- include "apex-common.configmap" . }}`
- [ ] `templates/service-monitor.yaml` -- `{{- include "apex-common.serviceMonitor" . }}`
- [ ] `templates/tests/test-connection.yaml` -- 포트 연결 테스트
- [ ] umbrella `Chart.yaml`에 의존성 추가 (`condition: <name>.enabled`)
- [ ] umbrella `values.yaml`에 `<name>.enabled: true/false` 추가
- [ ] umbrella `values-prod.yaml`에 프로덕션 오버라이드 추가
- [ ] `scripts/local-setup.sh`에서 2-release `--set` 플래그 갱신 (서비스면 infra release에 `--set <name>.enabled=false` 추가, 인프라면 services release에 추가)
- [ ] `helm dependency update .`
- [ ] `helm lint .`
- [ ] `helm template .` 으로 렌더링 확인

### 서비스 전용 리소스 추가 시

Ingress, HPA, PDB 등 일부 서비스에만 필요한 리소스는 해당 sub-chart의 `templates/`에 직접 작성한다. apex-common에 넣지 않는다. Gateway의 `ingress.yaml`, `hpa.yaml`, `pdb.yaml`을 참고.

---

## 4. 배포 절차

### 2-Release 순서

인프라가 먼저, 서비스가 나중. 서비스가 인프라에 의존하기 때문이다.

**설치**:

```bash
# 1. 의존성 갱신
helm dependency update .

# 2. 인프라 release
helm upgrade --install apex-infra . -n apex-infra --create-namespace \
  --set gateway.enabled=false \
  --set auth-svc.enabled=false \
  --set chat-svc.enabled=false \
  --wait --timeout 5m

# 3. 서비스 release
helm upgrade --install apex-services . -n apex-services --create-namespace \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false \
  --wait --timeout 5m

# 4. 테스트
helm test apex-services -n apex-services
```

**삭제**: 역순. 서비스 먼저, 인프라 나중.

```bash
helm uninstall apex-services -n apex-services
helm uninstall apex-infra -n apex-infra
```

### 검증 단계

차트 변경 후 반드시 수행:

| 단계 | 명령 | 목적 |
|------|------|------|
| lint | `helm lint .` | 구문 오류 |
| lint (prod) | `helm lint . -f values-prod.yaml` | 프로덕션 values 호환성 |
| template | `helm template apex-test .` | 렌더링 결과 확인 |
| dry-run | `helm install --dry-run apex-test . -n test` | 서버 측 검증 (클러스터 필요) |

---

## 5. Bitnami 의존성 관리

### 현재 의존성

| Chart | 버전 범위 | 용도 |
|-------|-----------|------|
| bitnami/kafka | `~32.x` | 메시징 |
| bitnami/redis (x4 alias) | `~21.x` | 캐시 |
| bitnami/postgresql | `~16.x` | RDBMS |

### 버전 업그레이드 방법

1. 릴리스 노트 확인: https://github.com/bitnami/charts/releases
2. `Chart.yaml`의 version 범위 변경 (예: `~32.x` -> `~33.x`)
3. `helm dependency update .` -- 새 버전 다운로드
4. `Chart.lock` 변경 확인 -- 실제 resolve된 버전
5. `helm lint .` -- 호환성 검증
6. `helm template .` -- 렌더링 결과에서 breaking change 확인
7. 로컬 환경에서 배포 테스트

### 주의사항

- Bitnami chart는 메이저 버전 간 breaking change가 잦다. values 스키마가 바뀌는 경우가 있으므로 반드시 릴리스 노트 확인
- `Chart.lock`은 `helm dependency update`가 자동 갱신. 수동 편집 금지
- Redis 4개 인스턴스는 같은 chart의 alias. 버전을 올리면 4개 모두 영향

---

## 6. 검증 필수

### 모든 차트 변경 후

```bash
# 구문 검사
helm lint .

# 프로덕션 values로도 검사
helm lint . -f values-prod.yaml

# 렌더링 확인 (서비스 release 시뮬레이션)
helm template apex-services . \
  --set kafka.enabled=false \
  --set redis-auth.enabled=false \
  --set redis-ratelimit.enabled=false \
  --set redis-chat.enabled=false \
  --set redis-pubsub.enabled=false \
  --set postgresql.enabled=false \
  --set pgbouncer.enabled=false

# 인프라 release 시뮬레이션
helm template apex-infra . \
  --set gateway.enabled=false \
  --set auth-svc.enabled=false \
  --set chat-svc.enabled=false
```

### apex-common 변경 시 추가 확인

`_helpers.tpl` 변경은 모든 서비스에 영향을 미친다. 개별 서비스 템플릿 렌더링을 각각 확인:

```bash
helm template . -s charts/gateway/templates/deployment.yaml
helm template . -s charts/auth-svc/templates/deployment.yaml
helm template . -s charts/chat-svc/templates/deployment.yaml
```

---

## 7. 금지 사항

### kubectl apply 직접 사용 금지

모든 K8s 리소스는 Helm chart를 통해 관리한다. `kubectl apply -f`로 chart 밖에서 리소스를 생성하면:
- Helm이 해당 리소스를 인식하지 못함
- `helm uninstall`로 정리되지 않음
- 리소스 충돌/중복 발생

### chart 외부에서 리소스 생성 금지

Namespace, ConfigMap, Secret, Service 등 모든 리소스는 `templates/` 또는 sub-chart `templates/` 안에서 정의한다. 별도 YAML 파일을 만들어 `kubectl apply`하는 것은 금지.

**예외**: 프로덕션 Secret (existingSecret 패턴). 이 Secret은 운영팀이 별도 프로세스로 관리하며, chart는 `existingSecret` 이름만 참조한다.

### Helm 외 도구로 chart 리소스 수정 금지

`kubectl edit`, `kubectl patch` 등으로 Helm이 관리하는 리소스를 직접 수정하면 다음 `helm upgrade` 시 덮어쓰기된다. 설정 변경은 values 파일을 수정하고 `helm upgrade`로 적용한다.

### sub-chart 간 직접 참조 금지

서비스 sub-chart가 다른 서비스 sub-chart의 내부 값을 직접 참조하지 않는다. 크로스 서비스 통신은 K8s DNS FQDN을 사용하며, 해당 주소는 각 서비스의 TOML ConfigMap에 설정한다.

### library chart에 서비스 전용 로직 넣지 않기

`apex-common`은 모든 서비스가 공유하는 공통 로직만 포함한다. Ingress, HPA, PDB 등 일부 서비스에만 필요한 리소스는 해당 sub-chart에 직접 작성한다.

### Chart.lock 수동 편집 금지

`Chart.lock`은 `helm dependency update`가 자동 생성/갱신한다. 직접 수정하면 의존성 무결성이 깨질 수 있다.

---

## 8. secrets 규칙

### 평문 시크릿 정책

| 파일 | 평문 시크릿 | 이유 |
|------|-------------|------|
| `values.yaml` | 허용 (개발용 기본값) | 로컬 minikube에서만 사용 |
| `values-prod.yaml` | **절대 금지** | Git에 커밋되므로 보안 위험 |
| `charts/<service>/values.yaml` | 허용 (개발용 기본값) | 로컬 fallback 용도 |

### 프로덕션 시크릿 패턴

프로덕션에서는 항상 `existingSecret` 패턴을 사용한다:

```yaml
# values-prod.yaml -- 올바른 예시
gateway:
  secrets:
    existingSecret: "apex-gateway-secrets"    # 운영팀이 사전 생성

rsaKeys:
  create: false
  existingSecret: "apex-rsa-keys"             # 운영팀이 사전 생성
```

운영팀이 Secret을 생성하는 방법은 chart 범위 밖이다 (kubectl create secret, Vault, External Secrets Operator 등).

### TOML 설정의 시크릿

TOML ConfigMap에는 `${VAR:-default}` 패턴만 사용한다. 실제 시크릿 값을 TOML에 하드코딩하지 않는다:

```toml
# 올바른 예시
password = "${REDIS_AUTH_PASSWORD:-dev_redis_pass}"

# 금지 -- 프로덕션 비밀번호를 TOML에 직접 넣지 않기
password = "production_actual_password"
```

### RSA 키

| 환경 | 설정 | 동작 |
|------|------|------|
| 로컬 | `rsaKeys.create: true` | `templates/secrets.yaml`에서 테스트 키로 Secret 자동 생성 |
| 프로덕션 | `rsaKeys.create: false`, `rsaKeys.existingSecret: "..."` | 외부 Secret 참조 |

테스트 RSA 키는 `values.yaml`에 내장되어 있으며, 이것은 로컬 개발/테스트 전용이다. 프로덕션에서는 반드시 별도 생성한 키를 사용한다.
