# docs/ 비정규 폴더 정리 + gh-pages 배포

**브랜치**: `feature/docs-cleanup-ghpages`
**백로그**: BACKLOG-214
**상태**: 완료

## 변경 요약

### 1. docs/ 비정규 폴더 정리 (26 파일 이동)

docs/ 루트에 `apex_` 접두어 없이 생성된 폴더 10개를 정규 모듈 폴더로 이동.

| 비정규 폴더 | 이동 대상 | 파일 수 |
|------------|----------|--------|
| `apex-agent/` | `apex_tools/` | 3 |
| `auto-review/` | `apex_common/` | 2 |
| `benchmark-report-v0.6.1/` | `apex_core/` | 1 |
| `docker-multistage-build/` | `apex_infra/` | 2 |
| `log-patterns-guide/` | `apex_core/` | 1 |
| `prometheus-metrics/` | `apex_core/` | 2 |
| `test-coverage/` | `apex_common/` | 4 |
| `v0.6.3-k8s-helm/` | `apex_infra/` | 4 |
| `showcase/` | `apex_common/showcase/` | 2 |
| `superpowers/` | `apex_core/plans/` + `apex_infra/plans/` 개별 분배 | 5 |

정리 후 docs/ 루트 디렉터리: `apex_common/`, `apex_core/`, `apex_infra/`, `apex_shared/`, `apex_tools/` (5개)

### 2. gh-pages 배포

`architecture_comparison.html` v0.6.1 데이터를 gh-pages 브랜치에 배포.
- `benchmark/index.html` — 아키텍처 비교 메인 페이지
- `docs/apex_core/benchmark/architecture_comparison.html` — 직접 경로
