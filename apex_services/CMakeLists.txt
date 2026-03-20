# Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

# apex_services — MSA services
add_subdirectory(gateway)
add_subdirectory(auth-svc)
add_subdirectory(chat-svc)

# E2E integration tests (requires docker-compose infrastructure)
if(BUILD_TESTING)
    add_subdirectory(tests/e2e)
endif()
