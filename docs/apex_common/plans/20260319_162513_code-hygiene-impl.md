# 코드 위생 확립 — 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** clang-format 도입 + 컴파일러 경고 전수 소탕으로 v0.6 진입 전 코드 위생 인프라 확립

**Architecture:** Phase 1(포맷팅)에서 `.clang-format` 설정 + 전체 포맷팅 + CI 강제를 완료한 뒤, Phase 2(경고)에서 CMake 경고 함수 + 전 타겟 적용 + 경고 수정을 진행한다. 포맷팅 커밋은 `.git-blame-ignore-revs`로 blame에서 격리.

**Tech Stack:** clang-format 18+, CMake, MSVC `/W4 /WX`, GCC/Clang `-Wall -Wextra -Wpedantic -Werror`, GitHub Actions

**Spec:** `docs/apex_common/plans/20260319_161800_code-hygiene-design.md`

---

## Phase 1: 포맷팅 (BACKLOG-58)

### Task 1: `.clang-format` + `.clang-format-ignore` 작성

**Files:**
- Create: `.clang-format`
- Create: `.clang-format-ignore`

- [ ] **Step 1: `.clang-format` 파일 생성**

```yaml
BasedOnStyle: LLVM
Language: Cpp
Standard: Latest

# Allman brace (람다 제외)
BreakBeforeBraces: Custom
BraceWrapping:
  AfterCaseLabel: true
  AfterClass: true
  AfterControlStatement: Always
  AfterEnum: true
  AfterFunction: true
  AfterNamespace: true
  AfterStruct: true
  AfterUnion: true
  AfterExternBlock: true
  BeforeCatch: true
  BeforeElse: true
  BeforeLambdaBody: false
  BeforeWhile: true
  SplitEmptyFunction: false
  SplitEmptyRecord: false
  SplitEmptyNamespace: false

# 레이아웃
ColumnLimit: 120
IndentWidth: 4
TabWidth: 4
UseTab: Never
IndentCaseLabels: true
NamespaceIndentation: None

# 생성자 이니셜라이저
BreakConstructorInitializers: BeforeComma
PackConstructorInitializers: Never

# 포인터/참조
PointerAlignment: Left

# include 정렬
SortIncludes: CaseInsensitive
IncludeBlocks: Preserve

# 기타
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
AlignTrailingComments: true
SpaceAfterCStyleCast: false
```

- [ ] **Step 2: `.clang-format-ignore` 파일 생성**

```
build/
vcpkg_installed/
*_generated.h
```

- [ ] **Step 3: 커밋**

```bash
git add .clang-format .clang-format-ignore
git commit -m "chore(infra): BACKLOG-58 clang-format 설정 파일 추가 (Allman, 120자, 람다 K&R 예외)"
git push
```

---

### Task 2: 전체 코드베이스 포맷팅

**Files:**
- Modify: `apex_core/` 전체 `.cpp`, `.hpp`, `.h`
- Modify: `apex_shared/` 전체 `.cpp`, `.hpp`, `.h`
- Modify: `apex_services/` 전체 `.cpp`, `.hpp`, `.h`

- [ ] **Step 1: 전체 포맷팅 실행**

```bash
find apex_core apex_shared apex_services \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
  ! -name '*_generated.h' \
  | xargs clang-format -i
```

- [ ] **Step 2: 포맷팅 결과 검토**

diff 규모 확인:
```bash
git diff --stat
```

의도치 않은 변환이 없는지 샘플 검토:
```bash
git diff apex_core/src/server.cpp
git diff apex_core/src/session.cpp
git diff apex_services/gateway/src/gateway_service.cpp
```

포맷팅에서 제외해야 할 파일이 발견되면 `.clang-format-ignore`에 추가 후 재실행.

- [ ] **Step 3: 빌드 + 테스트 통과 확인**

Run: `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug`
Expected: 71/71 테스트 통과 (포맷팅은 코드 의미를 변경하지 않으므로 반드시 통과해야 함)

- [ ] **Step 4: 포맷팅 커밋 (단독)**

```bash
git add -A
git commit -m "style: BACKLOG-58 clang-format 전체 적용 — Allman brace + 120자 통일"
git push
```

이 커밋 해시를 기록해둔다 (Step 5에서 사용).

---

### Task 3: blame 보존 설정

**Files:**
- Create: `.git-blame-ignore-revs`

- [ ] **Step 1: `.git-blame-ignore-revs` 파일 생성**

```bash
# Task 2 Step 4의 커밋 해시를 사용
HASH=$(git log --oneline -1 --format="%H" HEAD)
cat > .git-blame-ignore-revs << EOF
# clang-format 전체 포맷팅 (BACKLOG-58)
${HASH}
EOF
```

- [ ] **Step 2: 커밋**

```bash
git add .git-blame-ignore-revs
git commit -m "chore(infra): BACKLOG-58 .git-blame-ignore-revs 등록 — 포맷팅 커밋 blame 제외"
git push
```

---

### Task 4: CI format-check job 추가

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `apex_infra/docker/ci.Dockerfile` (clang-format 설치 추가)

- [ ] **Step 1: CI Docker 이미지에 clang-format 추가**

`apex_infra/docker/ci.Dockerfile`의 apt-get install 목록에 `clang-format-18` 추가:

```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-14 cmake ninja-build git curl zip unzip tar pkg-config ca-certificates python3 \
    make linux-libc-dev perl \
    autoconf automake libtool bison flex libreadline-dev \
    clang-format-18 \
    && rm -rf /var/lib/apt/lists/*
```

- [ ] **Step 2: ci.yml에 format-check job 추가**

`changes` job 바로 뒤, `build-image` job 전에 추가:

```yaml
  # ── Format check (clang-format) ────
  format-check:
    permissions:
      contents: read
    runs-on: ubuntu-latest
    needs: [changes]
    if: needs.changes.outputs.source == 'true'
    steps:
      - uses: actions/checkout@v4
      - name: Install clang-format
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends clang-format-18
      - name: Check formatting
        run: |
          find apex_core apex_shared apex_services \
            \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
            ! -name '*_generated.h' \
            | xargs clang-format-18 --dry-run --Werror
```

참고: format-check는 CI Docker 컨테이너 없이 `ubuntu-latest`에서 직접 실행. 빌드 불필요하므로 ~10초 완료.

- [ ] **Step 3: 커밋**

```bash
git add .github/workflows/ci.yml apex_infra/docker/ci.Dockerfile
git commit -m "ci: BACKLOG-58 clang-format CI 강제 — format-check job + Docker 이미지 갱신"
git push
```

---

## Phase 2: 경고 소탕 (BACKLOG-54)

### Task 5: CMake 경고 함수 정의 + 전 타겟 적용

**Files:**
- Modify: `CMakeLists.txt` (루트)
- Modify: `apex_core/CMakeLists.txt`
- Modify: `apex_core/tests/unit/CMakeLists.txt`
- Modify: `apex_core/tests/integration/CMakeLists.txt`
- Modify: `apex_core/examples/CMakeLists.txt`
- Modify: `apex_core/benchmarks/CMakeLists.txt`
- Modify: `apex_shared/CMakeLists.txt`
- Modify: `apex_shared/lib/protocols/kafka/CMakeLists.txt`
- Modify: `apex_shared/lib/protocols/websocket/CMakeLists.txt`
- Modify: `apex_shared/lib/adapters/common/CMakeLists.txt`
- Modify: `apex_shared/lib/adapters/kafka/CMakeLists.txt`
- Modify: `apex_shared/lib/adapters/redis/CMakeLists.txt`
- Modify: `apex_shared/lib/adapters/pg/CMakeLists.txt`
- Modify: `apex_shared/lib/rate_limit/CMakeLists.txt`
- Modify: `apex_shared/lib/protocols/kafka/tests/CMakeLists.txt`
- Modify: `apex_shared/tests/unit/CMakeLists.txt`
- Modify: `apex_shared/tests/integration/CMakeLists.txt`
- Modify: `apex_services/gateway/CMakeLists.txt`
- Modify: `apex_services/auth-svc/CMakeLists.txt`
- Modify: `apex_services/auth-svc/tests/CMakeLists.txt`
- Modify: `apex_services/chat-svc/CMakeLists.txt`
- Modify: `apex_services/chat-svc/tests/CMakeLists.txt`
- Modify: `apex_services/tests/e2e/CMakeLists.txt`

- [ ] **Step 1: 루트 CMakeLists.txt에 경고 함수 정의**

`CMakeLists.txt` (루트)에 `project()` 선언 이후에 추가:

```cmake
# ── 경고 정책 ──────────────────────────────────────────
function(apex_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /external:W0)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()
```

참고: `/external:W0`은 MSVC에서 SYSTEM include (vcpkg 외부 헤더)의 경고를 완전 억제한다.

- [ ] **Step 2: apex_core 타겟에 적용**

`apex_core/CMakeLists.txt`:
```cmake
apex_set_warnings(apex_core)
```

테스트/예제/벤치마크 타겟은 **헬퍼 함수 내부에 `apex_set_warnings()` 삽입**으로 일괄 적용:
```cmake
# apex_core/tests/unit/CMakeLists.txt의 apex_add_unit_test() 함수 내부에:
apex_set_warnings(${test_name})

# 동일하게 수정할 헬퍼 함수 목록:
# - apex_add_unit_test()          (apex_core/tests/unit/)
# - apex_add_integration_test()   (apex_core/tests/integration/)
# - apex_add_example()            (apex_core/examples/)
# - apex_add_benchmark()          (apex_core/benchmarks/)
# - apex_shared_add_unit_test()   (apex_shared/tests/unit/)
# - apex_shared_add_*_test()      (apex_shared/tests/unit/ 어댑터별)
# - apex_shared_add_integration_test() (apex_shared/tests/integration/)
```

`apex_core/benchmarks/CMakeLists.txt` — `apex_bench_main` OBJECT 라이브러리에도 적용:
```cmake
apex_set_warnings(apex_bench_main)
```

- [ ] **Step 3: apex_shared 타겟에 적용**

`apex_shared/CMakeLists.txt`:
```cmake
apex_set_warnings(apex_shared)
```

어댑터/프로토콜 서브 라이브러리 각각:
```cmake
# apex_shared/lib/protocols/kafka/CMakeLists.txt
apex_set_warnings(apex_protocols_kafka)

# apex_shared/lib/protocols/websocket/CMakeLists.txt
apex_set_warnings(apex_protocols_websocket)

# apex_shared/lib/adapters/common/CMakeLists.txt
apex_set_warnings(apex_shared_adapters_common)

# apex_shared/lib/adapters/kafka/CMakeLists.txt
apex_set_warnings(apex_shared_adapters_kafka)

# apex_shared/lib/adapters/redis/CMakeLists.txt
apex_set_warnings(apex_shared_adapters_redis)

# apex_shared/lib/adapters/pg/CMakeLists.txt
apex_set_warnings(apex_shared_adapters_pg)

# apex_shared/lib/rate_limit/CMakeLists.txt
apex_set_warnings(apex_shared_rate_limit)
```

`apex_shared/tests/unit/CMakeLists.txt`, `integration/CMakeLists.txt` — 헬퍼 함수 내부에 `apex_set_warnings()` 삽입 (Step 2와 동일 패턴).

`apex_shared/lib/protocols/kafka/tests/CMakeLists.txt` — `test_envelope_builder` 타겟에 적용.

참고: `apex_protocols_tcp`는 INTERFACE 라이브러리이므로 컴파일 옵션 불필요 — 스킵.

- [ ] **Step 4: apex_services 타겟에 적용**

```cmake
# apex_services/gateway/CMakeLists.txt
apex_set_warnings(apex_gateway)
apex_set_warnings(test_gateway_route_table)
apex_set_warnings(test_gateway_jwt_verifier)
apex_set_warnings(test_gateway_file_watcher)
apex_set_warnings(test_gateway_pending_requests)
apex_set_warnings(test_gateway_channel_session_map)
apex_set_warnings(test_gateway_message_router)
apex_set_warnings(test_gateway_pipeline)
apex_set_warnings(test_gateway_config_reloader)

# apex_services/auth-svc/CMakeLists.txt
# apex_bcrypt는 외부 C 라이브러리 — 경고 적용 제외
apex_set_warnings(apex_auth_svc)
apex_set_warnings(auth_svc_main)

# apex_services/auth-svc/tests/CMakeLists.txt
apex_set_warnings(auth_svc_unit_tests)

# apex_services/chat-svc/CMakeLists.txt
apex_set_warnings(apex_chat_svc)
apex_set_warnings(chat_svc_main)

# apex_services/chat-svc/tests/CMakeLists.txt
apex_set_warnings(chat_svc_unit_tests)
apex_set_warnings(chat_svc_handler_tests)

# apex_services/tests/e2e/CMakeLists.txt
apex_set_warnings(apex_e2e_tests)
```

- [ ] **Step 5: 커밋 (경고 함수 정의 + 적용만, 수정은 아직)**

```bash
git add CMakeLists.txt apex_core/ apex_shared/ apex_services/
git commit -m "chore(infra): BACKLOG-54 apex_set_warnings() 정의 + 전 타겟 적용"
git push
```

---

### Task 6: 경고 수집 + 전수 수정

**Files:**
- Modify: 경고가 발생하는 모든 소스 파일 (빌드 후 판단)

- [ ] **Step 1: 로컬 MSVC 빌드로 경고 수집**

Run: `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug`
빌드가 `-Werror`/`/WX`로 인해 실패할 것. 경고 목록을 수집한다.

- [ ] **Step 2: 경고 분류 및 수정 계획**

수집된 경고를 유형별로 분류:
- 미사용 변수/파라미터 (`C4100`, `-Wunused-parameter`)
- 암시적 타입 변환 (`C4244`, `-Wconversion`)
- sign 비교 (`C4389`, `-Wsign-compare`)
- switch fallthrough (`C4706`, `-Wimplicit-fallthrough`)
- GTest 매크로 관련 (`[[nodiscard]]` 등)
- 기타

유형별 건수를 파악하고 수정 전략을 수립한다.

- [ ] **Step 3: 경고 전수 수정**

원칙:
- 코드 수정 우선 (억제 최후의 수단)
- 미사용 파라미터: 함수 시그니처에서 파라미터명 제거 또는 `[[maybe_unused]]`
- 타입 변환: 명시적 `static_cast<>()` 또는 타입 일치 수정
- GTest `[[nodiscard]]`: `(void)` 캐스트 (`.github/CLAUDE.md` 기록된 패턴)
- 수정 불가 경고: `apex_set_warnings()` 함수에 컴파일러별 개별 비활성화 + 사유 주석

모듈별로 수정:
1. `apex_core/` — 코어 프레임워크
2. `apex_shared/` — 어댑터, 프로토콜, rate_limit
3. `apex_services/` — gateway, auth-svc, chat-svc, 테스트

- [ ] **Step 4: MSVC 빌드 통과 확인**

Run: `"<프로젝트루트절대경로>/apex_tools/queue-lock.sh" build debug`
Expected: 빌드 성공, 71/71 테스트 통과, 경고 0건

- [ ] **Step 5: 커밋**

```bash
git add -A
git commit -m "fix(all): BACKLOG-54 컴파일러 경고 전수 수정 — /W4 /WX + -Wall -Wextra -Wpedantic -Werror"
git push
```

---

### Task 7: CI 검증 + GCC 추가 경고 수정

- [ ] **Step 1: CI 빌드 확인**

PR을 생성하거나 push 후 CI 결과를 확인한다.
MSVC에서는 통과했지만 GCC/Clang에서만 발생하는 경고가 있을 수 있다.

```bash
gh run watch --exit-status
```

- [ ] **Step 2: GCC/Clang 전용 경고 수정**

CI에서 실패한 경고를 수정한다. 일반적인 GCC 전용 경고:
- `-Wsign-compare` (MSVC `/W4`에서는 미경고)
- `-Wimplicit-fallthrough` (GCC가 더 엄격)
- `SIZE_MAX` 미선언 (`.github/CLAUDE.md`: `<cstdint>` include 필수)

- [ ] **Step 3: ASAN/TSAN 프리셋 경고 확인**

CI의 `linux-asan`, `linux-tsan` job도 모두 통과하는지 확인.
Sanitizer 삽입 코드가 추가 경고를 유발할 수 있으므로 별도 확인 필요.

- [ ] **Step 4: 수정 후 재커밋 + CI 재확인**

```bash
git add -A
git commit -m "fix(all): BACKLOG-54 GCC/Clang 경고 추가 수정"
git push
```

CI 전체 통과까지 반복.

---

## 완료 후 절차

### Task 8: 머지 전 문서 갱신 + PR

- [ ] **Step 1: 문서 갱신**

머지 전 필수 갱신 대상:
- `docs/Apex_Pipeline.md` — 로드맵에 v0.5.7.0 항목 추가
- `CLAUDE.md` — 로드맵 현재 버전 갱신
- `README.md` — 필요시 갱신
- `docs/BACKLOG.md` — BACKLOG-58, BACKLOG-54 삭제 → `docs/BACKLOG_HISTORY.md`에 이전
- `docs/apex_common/progress/` — progress 문서 작성

- [ ] **Step 2: 최종 커밋 + PR 생성**

```bash
git add -A
git commit -m "docs: BACKLOG-58, BACKLOG-54 완료 — 문서 갱신"
git push
gh pr create --title "BACKLOG-58,54 코드 위생 확립 — clang-format + 경고 전수 소탕" --body "..."
```

- [ ] **Step 3: auto-review 실행**

auto-review 필요 여부 판단 후 실행.

- [ ] **Step 4: 머지 절차**

1. `queue-lock.sh merge acquire`
2. `git fetch origin main && git rebase origin/main`
3. `queue-lock.sh build debug` (rebase 후 재빌드)
4. `git push --force-with-lease`
5. `gh pr merge --squash --admin`
6. `queue-lock.sh merge release`
7. `apex_tools/cleanup-branches.sh --execute`
