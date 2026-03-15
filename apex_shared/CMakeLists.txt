# apex_shared — 공유 FlatBuffers 스키마 + C++ 유틸 라이브러리

find_package(flatbuffers CONFIG REQUIRED)

# --- FlatBuffers 스키마 컴파일 ---
set(APEX_SHARED_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/gateway_envelope.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/auth_request.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/auth_response.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/error_envelope.fbs
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

# --- Protocols ---
add_subdirectory(lib/protocols)

# --- Adapters ---
add_subdirectory(lib/adapters)

# --- Rate Limiting ---
add_subdirectory(lib/rate_limit)

# --- Tests ---
add_subdirectory(tests)
