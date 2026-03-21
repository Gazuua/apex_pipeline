// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <string>
#include <string_view>

namespace apex::shared
{

/// Expand ${VAR_NAME} and ${VAR_NAME:-default} patterns in a string value.
/// If the environment variable is not set:
///   - ${VAR}           -> keeps "${VAR}" as-is (no substitution)
///   - ${VAR:-fallback} -> substitutes with "fallback"
[[nodiscard]] std::string expand_env(std::string_view value);

} // namespace apex::shared
