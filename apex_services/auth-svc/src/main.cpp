#include <apex/auth_svc/auth_config.hpp>

#include <spdlog/spdlog.h>

#include <cstdlib>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    spdlog::info("Auth Service starting...");

    // TODO: TOML config loading
    // TODO: Server instance + adapter registration + AuthService start
    // Scaffolding -- build verification only

    spdlog::info("Auth Service placeholder -- exiting.");
    return EXIT_SUCCESS;
}
