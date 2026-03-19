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
        endif()
    endfunction()
endif()
