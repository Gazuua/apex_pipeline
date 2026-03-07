find_package(GTest CONFIG REQUIRED)

add_subdirectory(unit)
add_subdirectory(integration)
# add_subdirectory(bench)        # Phase 2 (after unit tests pass)
