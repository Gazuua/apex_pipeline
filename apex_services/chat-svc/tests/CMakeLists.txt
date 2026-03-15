find_package(GTest CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)

# --- Chat Service Unit Tests ---
add_executable(chat_svc_unit_tests
    chat_service_test.cpp
    chat_room_test.cpp
    chat_message_test.cpp
)

# Depend on FBS generation so generated headers exist
add_dependencies(chat_svc_unit_tests chat_svc_fbs_generate)

target_link_libraries(chat_svc_unit_tests
    PRIVATE
        apex_chat_svc
        flatbuffers::flatbuffers
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(chat_svc_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        # FlatBuffers generated headers
        ${CMAKE_CURRENT_BINARY_DIR}/../generated
)

if(WIN32)
    target_compile_options(chat_svc_unit_tests PRIVATE /utf-8)
    target_compile_definitions(chat_svc_unit_tests PRIVATE _WIN32_WINNT=0x0A00)
endif()

add_test(NAME chat_svc_unit_tests COMMAND chat_svc_unit_tests)
