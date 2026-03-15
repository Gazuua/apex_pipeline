#pragma once

#include <apex/gateway/gateway_config.hpp>
#include <apex/core/result.hpp>

#include <string_view>

namespace apex::gateway {

/// gateway.toml file parser.
/// @param path TOML file path
/// @return Parsed GatewayConfig or error
[[nodiscard]] apex::core::Result<GatewayConfig>
parse_gateway_config(std::string_view path);

} // namespace apex::gateway
