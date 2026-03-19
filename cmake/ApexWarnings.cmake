# ── 경고 정책 ──────────────────────────────────────────
# 모든 프로젝트 타겟에 적용하는 공통 경고 함수.
# 루트 CMakeLists.txt와 apex_core/CMakeLists.txt 양쪽에서 include.
if(NOT COMMAND apex_set_warnings)
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
            # GCC 14 + TSAN: 표준 라이브러리(atomic_base.h)에서 atomic_thread_fence 경고 발생.
            # 우리 코드가 아닌 GCC 자체 이슈이므로 tsan 빌드에서만 억제.
            if(APEX_BUILD_VARIANT STREQUAL "tsan")
                target_compile_options(${target} PRIVATE -Wno-tsan)
            endif()
        endif()
    endfunction()
endif()
