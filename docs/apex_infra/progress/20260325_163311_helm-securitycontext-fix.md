# helm-validation CreateContainerConfigError 수정

- **일시**: 2026-03-25
- **브랜치**: `bugfix/helm-securitycontext-fix`
- **백로그**: BACKLOG-230
- **PR**: #165

## 배경

PR #163에서 추가된 `securityContext(runAsNonRoot: true)`가 CI helm-validation 잡에서
`CreateContainerConfigError`를 유발. 3개 서비스 Pod(gateway, auth-svc, chat-svc) 전부
컨테이너 시작 자체에 실패.

## 원인

`minikube image load`로 이미지를 로딩할 때 Docker IMAGE의 USER 메타데이터가 불완전하게
보존됨. kubelet이 기본 실행 사용자를 판별하지 못하면 root(UID 0)로 간주하고,
`runAsNonRoot: true` 검증에 실패하여 `CreateContainerConfigError` 발생.

## 수정 내용

1. **Dockerfile UID 고정**: `useradd --uid 10001 --gid 10001` — deterministic UID로 이미지 메타데이터 의존 제거
2. **Helm runAsUser 명시**: `_helpers.tpl`에 `runAsUser: 10001`, `runAsGroup: 10001` 추가 — 이미지 메타데이터 없이도 runAsNonRoot 검증 통과
3. **CI debug 강화**: `kubectl describe pod` 추가 — CreateContainerConfigError 상세 원인 확인 가능

## 검증

- helm template 렌더링: 3서비스 securityContext 정상 출력
- values 오버라이드: `.Values.securityContext` 커스텀 정상 동작
- prod values 호환: 기본값 적용, 프로덕션 오버라이드 가능
