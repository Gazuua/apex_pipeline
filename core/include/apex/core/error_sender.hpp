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
    /// 에러 응답 프레임을 빌드합니다.
    /// @param original_msg_id 원본 메시지 ID (응답 대상)
    /// @param code 에러 코드
    /// @param message 에러 메시지 (빈 문자열 허용; FlatBuffers 필드가 null이 될 수 있음.
    ///                수신측에서 nullptr 체크 필요)
    /// @return 직렬화된 프레임 바이트 벡터
    [[nodiscard]] static std::vector<uint8_t> build_error_frame(
        uint16_t original_msg_id,
        ErrorCode code,
        std::string_view message = "");
};

} // namespace apex::core
