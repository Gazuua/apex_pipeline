# K8s Helm readOnlyRootFilesystem + file logging 충돌 수정

- **PR**: #203
- **백로그**: BACKLOG-255
- **브랜치**: bugfix/helm-readonly-fs-logging

## 문제

PR #202 auto-review에서 `_helpers.tpl`에 `readOnlyRootFilesystem: true` 추가.
서비스 TOML의 `[logging.file] enabled = true`가 `fs::create_directories(log_dir)` 호출 시
read-only 파일시스템 쓰기 실패 → 3개 서비스 Pod 크래시 → CI helm-validation 실패.

## 수정

4개 서비스 Helm values의 K8s TOML에서 `[logging.file] enabled = false` 설정:
- `charts/gateway/values.yaml`
- `charts/auth-svc/values.yaml`
- `charts/chat-svc/values.yaml`
- `charts/log-svc/values.yaml`

## 영향 범위

- K8s 환경만 해당 — 로컬(`e2e/*.toml`), Docker Compose(`e2e/docker/*_docker.toml`)는 변경 없음
- K8s 로그 수집은 Loki + Vector 스택으로 stdout 기반 수집 예정
