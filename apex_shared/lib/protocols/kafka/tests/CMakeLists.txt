find_package(GTest CONFIG REQUIRED)

add_executable(test_envelope_builder test_envelope_builder.cpp)
target_link_libraries(test_envelope_builder
    PRIVATE
        apex::shared::protocols::kafka
        apex::core
        GTest::gtest_main
)
add_test(NAME test_envelope_builder COMMAND test_envelope_builder)
set_tests_properties(test_envelope_builder PROPERTIES TIMEOUT 30)
