// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace apex::shared::adapters
{

/// 어댑터 에러 상세 컨텍스트.
/// ErrorCode::AdapterError와 함께 사용하여 라이브러리별 에러 정보를 전달.
struct AdapterError
{
    apex::core::ErrorCode code = apex::core::ErrorCode::AdapterError;
    uint32_t native_error = 0; ///< 라이브러리 원본 에러코드
    std::string message;       ///< 사람 읽기용 상세

    /// 에러 여부 확인
    [[nodiscard]] bool ok() const noexcept
    {
        return code == apex::core::ErrorCode::Ok;
    }

    /// 축약 문자열 (로깅용)
    [[nodiscard]] std::string to_string() const;
};

} // namespace apex::shared::adapters
