# apex_services/auth-svc -- Auth Service (v0.5.3.0)

find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)

# jwt-cpp: header-only, vcpkg provides jwt-cpp::jwt-cpp target
# libbcrypt: vcpkg provides bcrypt target
find_package(bcrypt CONFIG QUIET)

# bcrypt fallback: find_path for libbcrypt header
if(NOT bcrypt_FOUND)
    find_path(BCRYPT_INCLUDE_DIR "bcrypt/BCrypt.hpp")
    if(NOT BCRYPT_INCLUDE_DIR)
        message(FATAL_ERROR "libbcrypt not found. Install via vcpkg.")
    endif()
endif()

# --- FlatBuffers schema compilation ---
set(AUTH_SVC_SCHEMAS
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/login_request.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/login_response.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/logout_request.fbs
    ${CMAKE_CURRENT_SOURCE_DIR}/schemas/logout_response.fbs
)

set(AUTH_SVC_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${AUTH_SVC_FBS_GENERATED_DIR})

foreach(SCHEMA ${AUTH_SVC_SCHEMAS})
    get_filename_component(SCHEMA_NAME ${SCHEMA} NAME_WE)
    set(GENERATED_HEADER ${AUTH_SVC_FBS_GENERATED_DIR}/${SCHEMA_NAME}_generated.h)
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatbuffers::flatc --cpp -o ${AUTH_SVC_FBS_GENERATED_DIR} ${SCHEMA}
        DEPENDS ${SCHEMA}
        COMMENT "FlatBuffers (auth-svc): ${SCHEMA_NAME}.fbs"
    )
    list(APPEND AUTH_SVC_FBS_GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(auth_svc_fbs_generate DEPENDS ${AUTH_SVC_FBS_GENERATED_HEADERS})

# --- Library ---
add_library(apex_auth_svc STATIC
    src/auth_service.cpp
    src/password_hasher.cpp
    src/jwt_manager.cpp
    src/session_store.cpp
)
add_library(apex::services::auth ALIAS apex_auth_svc)

add_dependencies(apex_auth_svc auth_svc_fbs_generate)

target_include_directories(apex_auth_svc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${AUTH_SVC_FBS_GENERATED_DIR}
)

target_link_libraries(apex_auth_svc
    PUBLIC
        apex_core
        apex_shared
        apex_shared_adapters_kafka
        apex_shared_adapters_redis
        apex_shared_adapters_pg
        apex_protocols_kafka
        spdlog::spdlog
    PRIVATE
        jwt-cpp::jwt-cpp
        OpenSSL::SSL
        OpenSSL::Crypto
        flatbuffers::flatbuffers
)

# libbcrypt linking
if(bcrypt_FOUND AND TARGET bcrypt::bcrypt)
    target_link_libraries(apex_auth_svc PRIVATE bcrypt::bcrypt)
elseif(BCRYPT_INCLUDE_DIR)
    target_include_directories(apex_auth_svc PRIVATE ${BCRYPT_INCLUDE_DIR})
endif()

target_compile_features(apex_auth_svc PUBLIC cxx_std_23)

if(WIN32)
    target_compile_options(apex_auth_svc PUBLIC /utf-8)
    target_compile_definitions(apex_auth_svc PRIVATE _WIN32_WINNT=0x0A00)
endif()

# --- Executable ---
add_executable(auth_svc_main src/main.cpp)
target_link_libraries(auth_svc_main PRIVATE apex_auth_svc)
if(WIN32)
    target_compile_options(auth_svc_main PRIVATE /utf-8)
    target_compile_definitions(auth_svc_main PRIVATE _WIN32_WINNT=0x0A00)
endif()

# --- Tests ---
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
