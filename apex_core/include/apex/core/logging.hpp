#pragma once

#include <apex/core/config.hpp>

namespace apex::core {

/// LogConfig 기반 spdlog 로거 초기화.
/// "apex" (프레임워크) + "app" (서비스) 로거 생성.
/// main() 초반 1회 호출.
void init_logging(const LogConfig& config);

/// spdlog 정리. Server::run() 리턴 후 호출 (선택적).
void shutdown_logging();

} // namespace apex::core
