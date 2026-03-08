# apex_shared 빌드 인프라 구현 계획

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** apex_shared의 FlatBuffers 코드젠 파이프라인 + STATIC 라이브러리 빌드 인프라를 세팅하여, 향후 스키마와 공유 코드를 추가하면 바로 빌드되는 틀을 만든다.

**Architecture:** apex_core와 동일한 `foreach + add_custom_command` 패턴으로 flatc 코드젠 구성. STATIC 라이브러리로 빌드하되, 현재는 placeholder 소스만 포함. 루트 CMakeLists.txt에서 `add_subdirectory`로 연결.

**Tech Stack:** CMake, FlatBuffers (vcpkg), C++23

**설계 문서:** `apex_docs/plans/20260307_234047_apex-shared-build-infra-design.md`

**충돌 방지:** apex_core/ 하위 파일 수정 금지

---

### Task 1: placeholder 소스 파일 생성

**Files:**
- Create: `apex_shared/lib/src/placeholder.cpp`

**Step 1: placeholder.cpp 작성**

```cpp
// apex_shared 라이브러리 빌드용 placeholder
// 공유 유틸 코드 추가 시 이 파일을 교체하거나 삭제
```

빈 translation unit. STATIC 라이브러리 타겟에 최소 1개 소스가 필요하므로 존재.

---

### Task 2: CMakeLists.txt 작성

**Files:**
- Create: `apex_shared/CMakeLists.txt`

**Step 1: CMakeLists.txt 작성**

```cmake
# apex_shared — 공유 FlatBuffers 스키마 + C++ 유틸 라이브러리

find_package(flatbuffers CONFIG REQUIRED)

# --- FlatBuffers 스키마 컴파일 ---
set(APEX_SHARED_SCHEMAS
    # 스키마 추가 시 여기에 등록:
    # ${CMAKE_CURRENT_SOURCE_DIR}/schemas/common.fbs
    # ${CMAKE_CURRENT_SOURCE_DIR}/schemas/auth.fbs
)

set(APEX_SHARED_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${APEX_SHARED_FBS_GENERATED_DIR})

foreach(SCHEMA ${APEX_SHARED_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    set(GENERATED_HEADER ${APEX_SHARED_FBS_GENERATED_DIR}/${SCHEMA_NAME}_generated.h)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatbuffers::flatc --cpp -o ${APEX_SHARED_FBS_GENERATED_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers (shared): ${SCHEMA_NAME}.fbs"
    )
    list(APPEND APEX_SHARED_FBS_GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

if(APEX_SHARED_FBS_GENERATED_HEADERS)
    add_custom_target(apex_shared_fbs_generate DEPENDS ${APEX_SHARED_FBS_GENERATED_HEADERS})
endif()

# --- 라이브러리 ---
add_library(apex_shared STATIC
    lib/src/placeholder.cpp
    # 공유 유틸 소스 추가 시 여기에 등록
)
add_library(apex::shared ALIAS apex_shared)

if(TARGET apex_shared_fbs_generate)
    add_dependencies(apex_shared apex_shared_fbs_generate)
endif()

target_include_directories(apex_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(apex_shared
    PUBLIC
        flatbuffers::flatbuffers
)

target_compile_features(apex_shared PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_shared PUBLIC /utf-8)
endif()
```

핵심 포인트:
- `APEX_SHARED_SCHEMAS` 리스트가 비어있으면 코드젠 타겟이 생성되지 않음 (조건부 `if`)
- alias `apex::shared`로 서비스에서 `target_link_libraries(my_svc apex::shared)` 사용
- generated 디렉토리를 PUBLIC include에 포함하여 서비스에서 `#include "generated/xxx_generated.h"` 가능

---

### Task 3: 루트 CMakeLists.txt에 apex_shared 연결

**Files:**
- Modify: `CMakeLists.txt` (루트)

**Step 1: add_subdirectory 주석 해제 및 경로 수정**

변경 전:
```cmake
# add_subdirectory(apex_shared/lib)         # 향후 추가
```

변경 후:
```cmake
add_subdirectory(apex_shared)
```

---

### Task 4: 빌드 검증

**Step 1: CMake configure**

Run: `cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"` (루트에서 실행 시 apex_shared도 configure됨)

또는 직접:
```bash
cmake --preset debug 2>&1 | grep -E "apex_shared|Error"
```

Expected: configure 성공, apex_shared 타겟 인식

**Step 2: 빌드**

Expected: apex_shared.lib 생성, 에러 없음

---

### Task 5: README.md 갱신

**Files:**
- Modify: `apex_shared/README.md`

**Step 1: README 갱신**

```markdown
# apex_shared

서비스 간 공유 리소스.

## 구조

- `schemas/` — 공유 FlatBuffers 스키마 (.fbs)
- `lib/` — 공유 C++ 코드 (STATIC 라이브러리)
  - `include/apex/shared/` — 공유 헤더
  - `src/` — 구현

## FlatBuffers 네임스페이스 규칙

| 레이어 | 네임스페이스 | 위치 |
|--------|-------------|------|
| 프레임워크 | `apex.messages` | apex_core/schemas/ |
| 서비스 공유 | `apex.shared.<domain>` | apex_shared/schemas/ |

## 스키마 추가 방법

1. `schemas/` 에 `.fbs` 파일 작성 (namespace: `apex.shared.<domain>`)
2. `CMakeLists.txt`의 `APEX_SHARED_SCHEMAS` 리스트에 경로 추가
3. 빌드 시 `generated/<name>_generated.h` 자동 생성

## 서비스에서 사용

```cmake
target_link_libraries(my_service PRIVATE apex::shared)
```

```cpp
#include "generated/<schema>_generated.h"
```
```

---

### Task 6: .gitkeep 정리 + 커밋

**Step 1: lib/include 내 .gitkeep 제거** (placeholder.cpp가 디렉토리를 유지하므로 불필요)

```bash
git rm apex_shared/lib/include/apex/shared/.gitkeep
```

참고: `schemas/.gitkeep`는 유지 (스키마 파일이 아직 없으므로)

**Step 2: 커밋**

```bash
git add apex_shared/CMakeLists.txt \
        apex_shared/lib/src/placeholder.cpp \
        apex_shared/README.md \
        CMakeLists.txt
git commit -m "infra(apex_shared): FlatBuffers 코드젠 + STATIC 라이브러리 빌드 인프라"
```
