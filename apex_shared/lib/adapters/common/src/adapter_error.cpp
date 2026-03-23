// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

// apex_shared/lib/adapters/common/src/adapter_error.cpp
#include <apex/shared/adapters/adapter_error.hpp>

#include <apex/core/scoped_logger.hpp>

#include <sstream>

namespace apex::shared::adapters
{

namespace
{
apex::core::ScopedLogger s_logger{"AdapterError", apex::core::ScopedLogger::NO_CORE, "app"};
} // anonymous namespace

std::string AdapterError::to_string() const
{
    std::ostringstream oss;
    oss << apex::core::error_code_name(code);
    if (native_error != 0)
    {
        oss << " (native=" << native_error << ")";
    }
    if (!message.empty())
    {
        oss << ": " << message;
    }
    auto result = oss.str();
    s_logger.trace("to_string: {}", result);
    return result;
}

} // namespace apex::shared::adapters
