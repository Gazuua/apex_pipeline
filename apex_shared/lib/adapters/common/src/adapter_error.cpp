// apex_shared/lib/adapters/common/src/adapter_error.cpp
#include <apex/shared/adapters/adapter_error.hpp>

#include <sstream>

namespace apex::shared::adapters
{

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
    return oss.str();
}

} // namespace apex::shared::adapters
