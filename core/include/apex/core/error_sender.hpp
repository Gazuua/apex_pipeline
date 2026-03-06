#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/wire_header.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace apex::core {

/// 와이어 포맷 에러 응답 프레임 빌더.
class ErrorSender {
public:
    [[nodiscard]] static std::vector<uint8_t> build_error_frame(
        uint16_t original_msg_id,
        ErrorCode code,
        std::string_view message = "");
};

} // namespace apex::core
