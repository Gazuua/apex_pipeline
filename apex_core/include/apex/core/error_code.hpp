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
    HandlerException = 11,      // dispatch handler threw exception
    SendFailed = 12,            // async_send network write failure
    InsufficientData = 13,      // buffer has no complete frame (read more signal)

    // Application errors (1000-1999)
    AppError = 1000,

    // Adapter errors (2000+)
    AdapterError = 2000,
    PoolExhausted = 2001,       // PgPool retry limit exceeded
    CircuitOpen = 2002,         // CircuitBreaker OPEN state — call rejected
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
        case ErrorCode::HandlerException: return "HandlerException";
        case ErrorCode::SendFailed: return "SendFailed";
        case ErrorCode::InsufficientData: return "InsufficientData";
        case ErrorCode::AppError: return "AppError";
        case ErrorCode::AdapterError: return "AdapterError";
        case ErrorCode::PoolExhausted: return "PoolExhausted";
        case ErrorCode::CircuitOpen: return "CircuitOpen";
        default: return "Unknown";
    }
}

// M-6: Stream formatter for ErrorCode
inline std::ostream& operator<<(std::ostream& os, ErrorCode ec) {
    return os << error_code_name(ec);
}

} // namespace apex::core
