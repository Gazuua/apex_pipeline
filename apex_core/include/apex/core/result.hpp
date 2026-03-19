#pragma once

#include <apex/core/error_code.hpp>
#include <expected>

namespace apex::core
{

template <typename T = void> using Result = std::expected<T, ErrorCode>;

/// 핸들러에서 성공 반환용 헬퍼
inline Result<void> ok()
{
    return {};
}

/// 핸들러에서 에러 반환용 헬퍼
inline std::unexpected<ErrorCode> error(ErrorCode code)
{
    return std::unexpected(code);
}

} // namespace apex::core
