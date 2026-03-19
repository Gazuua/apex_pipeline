cmake_minimum_required(VERSION 3.25)
project(apex-pipeline
    VERSION 0.4.0
    LANGUAGES C CXX
    DESCRIPTION "Apex Pipeline - High-performance real-time server monorepo"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TODO: Linux io_uring 통합 시 활성화
option(APEX_USE_IO_URING "Enable io_uring backend for Boost.Asio" OFF)

enable_testing()

# ── 경고 정책 ──────────────────────────────────────────
function(apex_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /external:W0
            /wd4324  # C4324: struct padded due to alignas — 의도적 캐시라인 정렬
            /wd4099  # C4099: class/struct 불일치 — FlatBuffers 생성 코드에서 발생
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()

# --- Sub-projects ---
add_subdirectory(apex_core)
# 실제 코드 추가 시 활성화 (현재 placeholder만 존재)
add_subdirectory(apex_shared)
add_subdirectory(apex_services)
