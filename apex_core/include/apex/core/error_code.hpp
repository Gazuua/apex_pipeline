// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

namespace apex::core
{

enum class ErrorCode : uint16_t
{
    Ok = 0,

    // Framework errors (1-98) — 프레임워크 공통 에러
    Unknown = 1,
    InvalidMessage = 2,
    HandlerNotFound = 3,
    SessionClosed = 4,
    BufferFull = 5,
    Timeout = 6,
    FlatBuffersVerifyFailed = 7,
    CrossCoreTimeout = 8,
    CrossCoreQueueFull = 9,
    CrossCoreFuncException = 17, // cross_core_call target func threw exception
    BodyTooLarge = 18,           // frame body exceeds MAX_BODY_SIZE
    UnsupportedProtocolVersion = 10,
    HandlerException = 11, // dispatch handler threw exception
    SendFailed = 12,       // async_send network write failure
    InsufficientData = 13, // buffer has no complete frame (read more signal)
    AcceptFailed = 14,     // Transport::async_accept 실패
    HandshakeFailed = 15,  // TLS handshake 실패
    ParseFailed = 16,      // Envelope 파싱 실패

    // 서비스별 에러 sentinel — 핸들러가 에러 프레임을 직접 전송 완료했음을 의미.
    // connection_handler는 이 코드를 받으면 추가 에러 프레임을 보내지 않는다.
    // 실제 서비스 에러 코드는 ErrorResponse.service_error_code 필드로 전달.
    ServiceError = 99,
    // 100-999: 예약 (향후 프레임워크 확장)

    // Application errors (1000-1999)
    AppError = 1000,

    // Adapter errors (2000+)
    AdapterError = 2000,
    PoolExhausted = 2001, // PgPool retry limit exceeded
    CircuitOpen = 2002,   // CircuitBreaker OPEN state — call rejected
};

constexpr std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code)
    {
        case ErrorCode::Ok:
            return "Ok";
        case ErrorCode::Unknown:
            return "Unknown";
        case ErrorCode::InvalidMessage:
            return "InvalidMessage";
        case ErrorCode::HandlerNotFound:
            return "HandlerNotFound";
        case ErrorCode::SessionClosed:
            return "SessionClosed";
        case ErrorCode::BufferFull:
            return "BufferFull";
        case ErrorCode::Timeout:
            return "Timeout";
        case ErrorCode::FlatBuffersVerifyFailed:
            return "FlatBuffersVerifyFailed";
        case ErrorCode::CrossCoreTimeout:
            return "CrossCoreTimeout";
        case ErrorCode::CrossCoreQueueFull:
            return "CrossCoreQueueFull";
        case ErrorCode::CrossCoreFuncException:
            return "CrossCoreFuncException";
        case ErrorCode::BodyTooLarge:
            return "BodyTooLarge";
        case ErrorCode::UnsupportedProtocolVersion:
            return "UnsupportedProtocolVersion";
        case ErrorCode::HandlerException:
            return "HandlerException";
        case ErrorCode::SendFailed:
            return "SendFailed";
        case ErrorCode::InsufficientData:
            return "InsufficientData";
        case ErrorCode::AcceptFailed:
            return "AcceptFailed";
        case ErrorCode::HandshakeFailed:
            return "HandshakeFailed";
        case ErrorCode::ParseFailed:
            return "ParseFailed";
        case ErrorCode::ServiceError:
            return "ServiceError";
        case ErrorCode::AppError:
            return "AppError";
        case ErrorCode::AdapterError:
            return "AdapterError";
        case ErrorCode::PoolExhausted:
            return "PoolExhausted";
        case ErrorCode::CircuitOpen:
            return "CircuitOpen";
        default:
            return "Unknown";
    }
}

// M-6: Stream formatter for ErrorCode
inline std::ostream& operator<<(std::ostream& os, ErrorCode ec)
{
    return os << error_code_name(ec);
}

} // namespace apex::core
