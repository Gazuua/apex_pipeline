# 코드 위생 확립 — 설계 문서

**백로그**: BACKLOG-58 (코딩 컨벤션 + clang-format), BACKLOG-54 (경고 소탕 + 경고 레벨)
**버전**: v0.5.7.0
**브랜치**: `feature/58-54-code-hygiene`
**작성일**: 2026-03-19

---

## 1. 목표

v0.6 코드량 증가 전에 코드 위생 인프라를 확립한다:
- 통일된 코딩 스타일 강제 (clang-format)
- 컴파일러 경고 전수 소탕 + `-Werror`/`/WX` 격상
- CI 자동 차단으로 이후 위반 방지

## 2. 접근법

**Format-First** — 포맷팅을 먼저 수행하고, 경고 소탕을 그 위에서 진행한다.

근거:
- 포맷팅은 코드 의미를 변경하지 않으므로 별도 커밋으로 격리 가능
- 경고 수정은 코드 의미를 변경하므로 리뷰 시 변경 의도가 명확해야 함
- 포맷팅 후 정리된 코드 위에서 경고 수정 → 가독성 향상

## 3. Phase 1: 포맷팅 (BACKLOG-58)

### 3.1 `.clang-format` 설정

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

### 3.2 `.clang-format-ignore`

```
build/
vcpkg_installed/
*_generated.h
```

추가 제외 대상은 실제 포맷팅 수행 시 판단.

### 3.3 적용 절차

1. `.clang-format` + `.clang-format-ignore` 파일 작성
2. 전체 코드베이스 `clang-format -i` 실행
3. 결과 검토 — 의도치 않은 변환 발견 시 설정 조정 또는 제외 추가
4. 빌드 + 테스트 통과 확인
5. 포맷팅 커밋 (단독, 코드 변경과 섞지 않음)
6. `.git-blame-ignore-revs`에 포맷팅 커밋 해시 등록 → 별도 커밋

### 3.4 blame 보존

`.git-blame-ignore-revs` 파일:
```
# clang-format 전체 포맷팅 (BACKLOG-58)
<포맷팅 커밋 해시>
```

- GitHub: 이 파일을 자동 인식하여 blame UI에서 해당 커밋 건너뜀
- 로컬: `git config blame.ignoreRevsFile .git-blame-ignore-revs`

### 3.5 CI format-check job

ci.yml에 독립 job 추가:

```yaml
format-check:
  runs-on: ubuntu-latest
  needs: [changes]
  if: needs.changes.outputs.source == 'true'
  steps:
    - uses: actions/checkout@v4
    - name: Install clang-format
      run: |
        sudo apt-get update
        sudo apt-get install -y clang-format-18
    - name: Check formatting
      run: |
        find apex_core apex_shared apex_services \
          -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \
          | xargs clang-format-18 --dry-run --Werror
```

- 기존 `changes` job의 path filter 재활용
- 빌드 불필요, ~10초 완료
- 4개 빌드 구성과 독립 실행

## 4. Phase 2: 경고 소탕 (BACKLOG-54)

### 4.1 CMake 경고 함수

루트 `CMakeLists.txt`에 공통 함수 정의:

```cmake
function(apex_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()
```

모든 빌드 타겟에 적용:
```cmake
# 라이브러리/서비스 타겟
apex_set_warnings(apex_core)
apex_set_warnings(apex_gateway)
apex_set_warnings(apex_auth_svc)
# ... 전체 타겟

# 테스트/벤치마크 타겟에도 동일 적용
# GTest 매크로 경고(예: [[nodiscard]] 관련)는 개별 억제로 대응
apex_set_warnings(apex_core_tests)
# ... 전체 테스트 타겟
```

**테스트 타겟 방침**: 테스트 코드에도 동일하게 `/W4 /WX` + `-Werror` 적용. GTest 매크로에서 발생하는 경고(예: GCC `[[nodiscard]]` — `.github/CLAUDE.md` 기록)는 `(void)` 캐스트 등 코드 수정으로 우선 해결하고, 불가피한 경우만 개별 억제.

### 4.2 외부 헤더 경고 억제

vcpkg `find_package()`는 대부분 SYSTEM include 자동 처리. 누락 시 수동 추가:
```cmake
target_include_directories(${target} SYSTEM PRIVATE ${외부경로})
```

MSVC 추가 설정:
```cmake
if(MSVC)
    target_compile_options(${target} PRIVATE /external:W0)
endif()
```

### 4.3 경고 수정 원칙

- **코드 수정 우선** — 최대한 경고의 근본 원인을 수정
- **억제는 최후의 수단** — 수정 불가능한 경우(외부 매크로 전개 등)에만 명시적 비활성화 + 사유 주석
- **크로스 컴파일러** — 로컬(MSVC)에서 먼저 수정, CI(GCC)에서 추가 발견분 수정. 전 구성 동일 기준

### 4.4 적용 절차

1. 루트 CMakeLists.txt에 `apex_set_warnings()` 정의
2. 모든 타겟에 함수 적용 + SYSTEM include 확인
3. 빌드 → 경고 수집 및 분류
4. 경고 전수 수정
5. 수정 불가 경고 명시적 억제 + 사유 주석
6. 로컬 빌드 + CI 전체 통과 확인 (GCC/ASAN/TSAN/MSVC)

## 5. 작업 순서 요약

| 순서 | 내용 | 커밋 단위 |
|------|------|-----------|
| 1 | `.clang-format` + `.clang-format-ignore` 작성 | 커밋 1 |
| 2 | 전체 코드베이스 포맷팅 | 커밋 2 (blame-ignore 대상) |
| 3 | `.git-blame-ignore-revs` 등록 | 커밋 3 |
| 4 | CI `format-check` job 추가 | 커밋 4 |
| 5 | CMake 경고 함수 + 타겟 적용 | 커밋 5 |
| 6 | 경고 전수 수정 | 커밋 6 (규모에 따라 분할 가능) |

## 6. 리스크 및 대응

| 리스크 | 영향 | 대응 |
|--------|------|------|
| 포맷팅 후 의도치 않은 코드 변경 | 컴파일 실패 또는 동작 변경 | clang-format은 코드 의미를 안 바꿈. 빌드+테스트로 확인 |
| 경고 수가 예상보다 많음 | 작업량 증가 | 먼저 빌드해서 규모 파악 후 진행 |
| GCC/MSVC 경고 차이 | 한쪽에서만 발생하는 경고 | 로컬(MSVC) 먼저, CI(GCC) 추가분 수정 |
| 외부 헤더 경고 누수 | vcpkg 헤더 경고 | SYSTEM include 누락분 추가 |
| CI clang-format 버전 차이 | 로컬(21)과 CI(18) 포맷 결과 차이 | 포맷팅 적용 시 양 버전으로 결과 동일성 검증. 차이 발견 시 CI 버전을 최신으로 올리거나 로컬 버전을 고정 |
| Sanitizer + `-Werror` 상호작용 | ASAN/TSAN 프리셋에서 sanitizer 삽입 코드가 추가 경고 유발 가능 | Phase 2 빌드 시 debug뿐 아니라 ASAN/TSAN 프리셋도 함께 검증 |

## 7. 현재 코드베이스 현황

| 항목 | 수치 |
|------|------|
| C++ 파일 수 | 275개 |
| 총 라인 수 | ~35,900줄 |
| 현재 brace style | K&R 기반 (90%+, 생성자 이니셜라이저 등 일부 Allman 혼용) |
| 인덴트 | 4 스페이스 |
| 경고 플래그 | 미설정 (`/utf-8`만) |
| CI 빌드 구성 | GCC / ASAN / TSAN / MSVC |
