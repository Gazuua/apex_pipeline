#pragma once

#include <cstdint>
#include <string_view>

namespace apex::core {

enum class ErrorCode : uint16_t {
    Ok = 0,

    // 프레임워크 에러 (1-999)
    Unknown = 1,
    InvalidMessage = 2,
    HandlerNotFound = 3,
    SessionClosed = 4,
    BufferFull = 5,
    Timeout = 6,
    FlatBuffersVerifyFailed = 7,

    // 어플리케이션 에러 (1000+)
    AppError = 1000,
};

constexpr std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidMessage: return "InvalidMessage";
        case ErrorCode::HandlerNotFound: return "HandlerNotFound";
        case ErrorCode::SessionClosed: return "SessionClosed";
        case ErrorCode::BufferFull: return "BufferFull";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::FlatBuffersVerifyFailed: return "FlatBuffersVerifyFailed";
        case ErrorCode::AppError: return "AppError";
        default: return "Unknown";
    }
}

} // namespace apex::core
