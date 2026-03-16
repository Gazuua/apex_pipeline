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
    AcceptFailed = 14,           // Transport::async_accept 실패
    HandshakeFailed = 15,        // TLS handshake 실패
    ParseFailed = 16,            // Envelope 파싱 실패

    // Gateway errors (100-199)
    ConfigParseFailed = 100,     // TOML config 파싱 실패
    JwtVerifyFailed = 101,       // JWT 시그니처 검증 실패
    JwtExpired = 102,            // JWT 만료
    JwtBlacklisted = 103,        // JWT 블랙리스트
    RouteNotFound = 104,         // msg_id에 대한 라우팅 규칙 없음
    ServiceTimeout = 105,        // 서비스 응답 타임아웃
    PendingMapFull = 106,        // Pending requests map 포화
    RateLimitedIp = 107,         // Per-IP rate limit 초과
    RateLimitedUser = 108,       // Per-User rate limit 초과
    RateLimitedEndpoint = 109,   // Per-Endpoint rate limit 초과
    SubscriptionLimitExceeded = 110,  // Per-session 구독 상한 초과

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
        case ErrorCode::AcceptFailed: return "AcceptFailed";
        case ErrorCode::HandshakeFailed: return "HandshakeFailed";
        case ErrorCode::ParseFailed: return "ParseFailed";
        case ErrorCode::ConfigParseFailed: return "ConfigParseFailed";
        case ErrorCode::JwtVerifyFailed: return "JwtVerifyFailed";
        case ErrorCode::JwtExpired: return "JwtExpired";
        case ErrorCode::JwtBlacklisted: return "JwtBlacklisted";
        case ErrorCode::RouteNotFound: return "RouteNotFound";
        case ErrorCode::ServiceTimeout: return "ServiceTimeout";
        case ErrorCode::PendingMapFull: return "PendingMapFull";
        case ErrorCode::RateLimitedIp: return "RateLimitedIp";
        case ErrorCode::RateLimitedUser: return "RateLimitedUser";
        case ErrorCode::RateLimitedEndpoint: return "RateLimitedEndpoint";
        case ErrorCode::SubscriptionLimitExceeded: return "SubscriptionLimitExceeded";
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
