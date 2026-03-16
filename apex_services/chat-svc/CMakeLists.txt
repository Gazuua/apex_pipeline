# apex_services/chat-svc -- Chat Service (v0.5.3.1)

find_package(spdlog CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# --- FlatBuffers schema compilation ---
set(CHAT_SVC_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/chat_room.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/chat_message.fbs
)

set(CHAT_SVC_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CHAT_SVC_FBS_GENERATED_DIR})

foreach(SCHEMA ${CHAT_SVC_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    set(GENERATED_HEADER ${CHAT_SVC_FBS_GENERATED_DIR}/${SCHEMA_NAME}_generated.h)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatbuffers::flatc --cpp -o ${CHAT_SVC_FBS_GENERATED_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers (chat-svc): ${SCHEMA_NAME}.fbs"
    )
    list(APPEND CHAT_SVC_FBS_GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(chat_svc_fbs_generate DEPENDS ${CHAT_SVC_FBS_GENERATED_HEADERS})

# --- Library ---
add_library(apex_chat_svc STATIC
    src/chat_service.cpp
    src/chat_db_consumer.cpp
)
add_library(apex::services::chat ALIAS apex_chat_svc)

add_dependencies(apex_chat_svc chat_svc_fbs_generate)

target_include_directories(apex_chat_svc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CHAT_SVC_FBS_GENERATED_DIR}
)

target_link_libraries(apex_chat_svc
    PUBLIC
        apex_core
        apex_shared
        apex_shared_adapters_kafka
        apex_shared_adapters_redis
        apex_shared_adapters_pg
        apex_protocols_kafka
        spdlog::spdlog
    PRIVATE
        flatbuffers::flatbuffers
)

target_compile_features(apex_chat_svc PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_chat_svc PUBLIC /utf-8)
    target_compile_definitions(apex_chat_svc PRIVATE _WIN32_WINNT=0x0A00)
endif()

# --- Executable ---
add_executable(chat_svc_main src/main.cpp)
target_link_libraries(chat_svc_main PRIVATE apex_chat_svc tomlplusplus::tomlplusplus)
if(WIN32)
    target_compile_options(chat_svc_main PRIVATE /utf-8)
    target_compile_definitions(chat_svc_main PRIVATE _WIN32_WINNT=0x0A00)
endif()

# --- Tests ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
