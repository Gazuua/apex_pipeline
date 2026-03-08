#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace apex::core {

enum class ErrorCode : uint16_t {
    Ok = 0,

    // Framework errors (1-999)
    Unknown = 1,
    InvalidMessage = 2,
    HandlerNotFound = 3,
    SessionClosed = 4,
    BufferFull = 5,
    Timeout = 6,
    FlatBuffersVerifyFailed = 7,
    CrossCoreTimeout = 8,
    CrossCoreQueueFull = 9,
    UnsupportedProtocolVersion = 10,

    // Application errors (1000+)
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
        case ErrorCode::CrossCoreTimeout: return "CrossCoreTimeout";
        case ErrorCode::CrossCoreQueueFull: return "CrossCoreQueueFull";
        case ErrorCode::UnsupportedProtocolVersion: return "UnsupportedProtocolVersion";
        case ErrorCode::AppError: return "AppError";
        default: return "Unknown";
    }
}

// M-6: Stream formatter for ErrorCode
inline std::ostream& operator<<(std::ostream& os, ErrorCode ec) {
    return os << error_code_name(ec);
}

} // namespace apex::core
