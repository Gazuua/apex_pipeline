# 문서 경로 갱신 + 디렉토리 스캐폴딩 완료

**일자:** 2026-03-07
**커밋:** `720b27d`, `4e7222c`
**계획서:** `apex_docs/plans/20260307_200000_docs-path-update-and-scaffolding.md`

---

## 1. 문서 경로 갱신 (`720b27d`)

### 배경
apex_core 내부 `core/` 중첩 디렉토리 플래트닝(6c84a11) 이후, 문서에 옛 경로가 잔존하고 있었음.

### 작업 내용
- **대상:** 22개 .md 파일 (apex_core/docs/ 20개 + apex_docs/ 2개)
- **치환 건수:** 534건
- **치환 규칙** (긴 패턴 우선 적용):

| 변경 전 | 변경 후 |
|---------|---------|
| `core/include/apex/core/` | `include/apex/core/` |
| `core/include/` | `include/` |
| `core/src/` | `src/` |
| `core/tests/` | `tests/` |
| `core/examples/` | `examples/` |
| `core/schemas/` | `schemas/` |
| `core/CMakeLists.txt` | `CMakeLists.txt` |

- **추가 수동 갱신:**
  - `apex_docs/plans/20260307_174528_directory-structure-design.md` — 디렉토리 트리 플랫 구조로 갱신, CMake 참조 경로 수정
  - `apex_docs/progress/20260307_184424_monorepo-restructuring.md` — 미완료 항목 완료 처리

### 검증
잔존 옛 경로 0건 확인 (설명 텍스트 내 백틱 인용 제외).

---

## 2. 디렉토리 스캐폴딩 (`4e7222c`)

### 배경
설계 문서(`apex_docs/plans/20260307_174528_directory-structure-design.md`)에 명시된 4개 루트 디렉토리가 미생성 상태였음.

### 생성 구조

```
apex_services/
  gateway/          include/apex/gateway/  src/  tests/
  auth-svc/         include/apex/auth_svc/ src/  tests/
  chat-svc/         include/apex/chat_svc/ src/  tests/
  log-svc/          include/apex/log_svc/  src/  tests/

apex_shared/
  schemas/
  lib/              include/apex/shared/   src/

apex_infra/
  k8s/              gateway/  auth-svc/  chat-svc/  log-svc/

apex_tools/
```

- **신규 파일:** 24개 (README 4개 + .gitkeep 20개)
- **네이밍:** 디렉토리 kebab-case (`auth-svc`), C++ 네임스페이스 snake_case (`auth_svc`)

### 검증
`find` 출력이 계획서 예상 결과와 일치 확인.

---

## 남은 미완료 항목

| # | 항목 | 담당 |
|---|------|------|
| 3 | apex_core v0.2.2 코드 리뷰 Important 5건 수정 | 별도 에이전트 |
| 4 | MEMORY 파일 동기화 | 루트 관리 |
