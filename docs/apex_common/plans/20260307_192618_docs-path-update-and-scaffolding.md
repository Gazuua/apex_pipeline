# 문서 경로 갱신 + 디렉토리 스캐폴딩 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** core/ 플래트닝 후 남아있는 문서 내 옛 경로를 갱신하고, 설계 문서에 명시된 미생성 디렉토리를 스캐폴딩한다.

**Architecture:** Task 1은 `apex_core/docs/` 20개 파일 + `apex_docs/` 2개 파일의 `core/include/`, `core/src/` 등 옛 경로를 플래트닝 이후 경로로 일괄 치환한다. Task 2는 `apex_services/`, `apex_shared/`, `apex_infra/`, `apex_tools/`를 설계 문서 기준으로 생성한다.

**Scope:** 루트 디렉토리 관리 + Git 관리 (apex_core 코드 변경 없음)

---

## Task 1: 문서 경로 갱신 (apex_core/docs/ + apex_docs/)

### 배경

`apex_core/` 내부에 `core/` 중첩 디렉토리가 있었으나 플래트닝으로 제거됨:
- `core/include/apex/core/` → `include/apex/core/`
- `core/src/` → `src/`
- `core/tests/` → `tests/`
- `core/examples/` → `examples/`
- `core/schemas/` → `schemas/`
- `core/CMakeLists.txt` → `CMakeLists.txt` (루트 CMake와 구분 필요)

**대상 파일:** 21개 (524건)
- `apex_core/docs/design-decisions.md` (1건) — 현행 설계 문서
- `apex_core/docs/plans/` (8개 파일, 387건) — 과거 계획서
- `apex_core/docs/progress/` (6개 파일, 78건) — 과거 완료 기록
- `apex_core/docs/review/` (4개 파일, 58건) — 과거 리뷰 보고서
- `apex_docs/plans/20260307_175218_directory-structure-design.md` — 모노레포 구조 설계서
- `apex_docs/progress/20260307_191204_monorepo-restructuring.md` — 진행 현황

### Step 1: 치환 규칙 정의 및 드라이런

치환 규칙 (순서 중요 — 긴 패턴 우선):
```
core/include/apex/core/  →  include/apex/core/
core/include/             →  include/
core/src/                 →  src/
core/tests/               →  tests/
core/examples/            →  examples/
core/schemas/             →  schemas/
core/CMakeLists.txt       →  CMakeLists.txt  (※ 문맥에 따라 수동 확인 필요)
```

실행:
```bash
# 드라이런 — 변경될 라인 수 확인 (실제 수정 안 함)
cd D:/.workspace
for pat in \
  "core/include/apex/core/" \
  "core/include/" \
  "core/src/" \
  "core/tests/" \
  "core/examples/" \
  "core/schemas/"; do
  echo "=== $pat ==="
  grep -r --include="*.md" -c "$pat" apex_core/docs/ apex_docs/ 2>/dev/null | grep -v ":0$"
done
```

Expected: 각 패턴별 매칭 파일과 건수가 출력됨. 총합이 약 524건.

### Step 2: 일괄 치환 실행 (apex_core/docs/)

```bash
cd D:/.workspace

# sed로 일괄 치환 (긴 패턴 우선 적용)
find apex_core/docs -name "*.md" -exec sed -i \
  -e 's|core/include/apex/core/|include/apex/core/|g' \
  -e 's|core/include/|include/|g' \
  -e 's|core/src/|src/|g' \
  -e 's|core/tests/|tests/|g' \
  -e 's|core/examples/|examples/|g' \
  -e 's|core/schemas/|schemas/|g' \
  -e 's|core/CMakeLists\.txt|CMakeLists.txt|g' \
  {} +
```

### Step 3: 설계 문서 수동 갱신 (apex_docs/)

**파일:** `apex_docs/plans/20260307_175218_directory-structure-design.md`

수동 갱신 필요 사항:
1. **디렉토리 트리 (line 34-41):** `apex_core/core/` 중첩 제거, 플랫 구조로 갱신
2. **CMake 참조 (line 109):** `../../apex_core/core` → `../../apex_core`

**파일:** `apex_docs/progress/20260307_191204_monorepo-restructuring.md`

수동 갱신 필요 사항:
1. **미완료 항목 #2 (line 55-56):** "확인 및 수정 필요" → 완료 처리

### Step 4: 검증

```bash
# 잔존 확인 — 0건이어야 함
grep -r --include="*.md" -E "core/(include|src|tests|examples|schemas)/" \
  apex_core/docs/ apex_docs/ | head -20
```

Expected: 출력 없음 (0건)

### Step 5: 커밋

```bash
git add apex_core/docs/ apex_docs/
git commit -m "docs: core/ 플래트닝 후 문서 내 경로 일괄 갱신 (21파일, 524건)"
```

---

## Task 2: 디렉토리 스캐폴딩

### 배경

설계 문서(`apex_docs/plans/20260307_175218_directory-structure-design.md`)에 명시된 4개 루트 디렉토리가 미생성 상태:
- `apex_services/` — MSA 서비스들
- `apex_shared/` — 공유 FlatBuffers 스키마 + 공유 C++ 코드
- `apex_infra/` — Docker, K8s 인프라
- `apex_tools/` — CLI, 스크립트

### Step 1: 디렉토리 생성

```bash
cd D:/.workspace

# apex_services/ — 서비스별 표준 구조
for svc in gateway auth-svc chat-svc log-svc; do
  mkdir -p "apex_services/$svc/include/apex/${svc//-/_}"
  mkdir -p "apex_services/$svc/src"
  mkdir -p "apex_services/$svc/tests"
done

# apex_shared/ — 스키마 + 공유 라이브러리
mkdir -p apex_shared/schemas
mkdir -p apex_shared/lib/include/apex/shared
mkdir -p apex_shared/lib/src

# apex_infra/ — 인프라
mkdir -p apex_infra/k8s/gateway
mkdir -p apex_infra/k8s/auth-svc
mkdir -p apex_infra/k8s/chat-svc
mkdir -p apex_infra/k8s/log-svc

# apex_tools/
mkdir -p apex_tools
```

### Step 2: .gitkeep 배치 (빈 디렉토리 추적)

```bash
cd D:/.workspace

# 빈 leaf 디렉토리에 .gitkeep 추가
find apex_services apex_shared apex_infra apex_tools -type d -empty \
  -exec touch {}/.gitkeep \;
```

### Step 3: 최소 README 생성

각 루트 디렉토리에 역할 설명 README 배치:

**apex_services/README.md:**
```markdown
# apex_services

MSA 서비스 디렉토리. 각 서비스는 독립 빌드(`vcpkg.json` + `CMakeLists.txt` + `Dockerfile`)를 가진다.

| 서비스 | 역할 |
|--------|------|
| gateway | WebSocket/HTTP 게이트웨이 |
| auth-svc | 인증/인가 |
| chat-svc | 채팅 로직 |
| log-svc | 로그 수집/저장 |
```

**apex_shared/README.md:**
```markdown
# apex_shared

서비스 간 공유 리소스.

- `schemas/` — 공유 FlatBuffers 스키마 (.fbs)
- `lib/` — 공유 C++ 코드 (CMake 라이브러리)
```

**apex_infra/README.md:**
```markdown
# apex_infra

인프라 및 배포 설정.

- `docker-compose.yml` — 로컬 개발 환경 (프로파일: minimal / observability / full)
- `k8s/` — Helm charts (서비스별)
```

**apex_tools/README.md:**
```markdown
# apex_tools

개발 도구 및 스크립트.

- `new-service.sh` — 서비스 스캐폴딩 스크립트 (예정)
```

### Step 4: 진행 현황 문서 갱신

**파일:** `apex_docs/progress/20260307_191204_monorepo-restructuring.md`

미완료 항목 #1 "디렉토리 스캐폴딩" → 완료 처리

### Step 5: 검증

```bash
# 생성된 디렉토리 구조 확인
find apex_services apex_shared apex_infra apex_tools -type f | sort
```

Expected:
```
apex_infra/README.md
apex_infra/k8s/auth-svc/.gitkeep
apex_infra/k8s/chat-svc/.gitkeep
apex_infra/k8s/gateway/.gitkeep
apex_infra/k8s/log-svc/.gitkeep
apex_services/README.md
apex_services/auth-svc/include/apex/auth_svc/.gitkeep
apex_services/auth-svc/src/.gitkeep
apex_services/auth-svc/tests/.gitkeep
apex_services/chat-svc/include/apex/chat_svc/.gitkeep
apex_services/chat-svc/src/.gitkeep
apex_services/chat-svc/tests/.gitkeep
apex_services/gateway/include/apex/gateway/.gitkeep
apex_services/gateway/src/.gitkeep
apex_services/gateway/tests/.gitkeep
apex_services/log-svc/include/apex/log_svc/.gitkeep
apex_services/log-svc/src/.gitkeep
apex_services/log-svc/tests/.gitkeep
apex_shared/README.md
apex_shared/lib/include/apex/shared/.gitkeep
apex_shared/lib/src/.gitkeep
apex_shared/schemas/.gitkeep
apex_tools/README.md
```

### Step 6: 커밋

```bash
git add apex_services/ apex_shared/ apex_infra/ apex_tools/
git add apex_docs/progress/20260307_191204_monorepo-restructuring.md
git commit -m "chore: 모노레포 디렉토리 스캐폴딩 — services, shared, infra, tools"
```

---

## 작업 순서

```
Task 1: 문서 경로 갱신
  Step 1: 드라이런 ──→ Step 2: 일괄 치환 ──→ Step 3: 수동 갱신 ──→ Step 4: 검증 ──→ Step 5: 커밋
                                                                                          |
Task 2: 디렉토리 스캐폴딩                                                                  v
  Step 1: mkdir ──→ Step 2: .gitkeep ──→ Step 3: README ──→ Step 4: 문서 갱신 ──→ Step 5: 검증 ──→ Step 6: 커밋
```

총 2개 커밋, apex_core 소스 코드 변경 없음.
