// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/wire_header.hpp>

#include <flatbuffers/flatbuffers.h>
#include <generated/error_response_generated.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <span>

using namespace apex::core;

namespace
{

/// Helper: parse an error frame and return the ErrorResponse root.
const apex::messages::ErrorResponse* parse_error_response(const std::vector<uint8_t>& frame,
                                                          WireHeader* out_header = nullptr)
{
    auto header = WireHeader::parse(frame);
    if (!header.has_value())
        return nullptr;
    if (out_header)
        *out_header = *header;
    auto payload = std::span<const uint8_t>(frame.data() + WireHeader::SIZE, header->body_size);
    return flatbuffers::GetRoot<apex::messages::ErrorResponse>(payload.data());
}

} // namespace

TEST(ErrorSenderServiceCode, ServiceErrorCodeZero_Default)
{
    // Default parameter (service_error_code omitted) → field is 0
    auto frame = ErrorSender::build_error_frame(0x0001, ErrorCode::Unknown, "test");
    auto* resp = parse_error_response(frame);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->service_error_code(), 0);
}

TEST(ErrorSenderServiceCode, ServiceErrorCodeNonZero_RoundTrip)
{
    auto frame = ErrorSender::build_error_frame(0x0010, ErrorCode::AppError, "app error", 1234);
    auto* resp = parse_error_response(frame);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->service_error_code(), 1234);
}

TEST(ErrorSenderServiceCode, EmptyMessage_WithServiceCode)
{
    auto frame = ErrorSender::build_error_frame(0x0002, ErrorCode::HandlerNotFound, "", 500);
    auto* resp = parse_error_response(frame);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->service_error_code(), 500);
    EXPECT_EQ(resp->code(), static_cast<uint16_t>(ErrorCode::HandlerNotFound));
    // Empty message: FlatBuffers still creates a valid string
    ASSERT_NE(resp->message(), nullptr);
    EXPECT_STREQ(resp->message()->c_str(), "");
}

TEST(ErrorSenderServiceCode, AllFieldsCombined)
{
    const uint32_t msg_id = 0xDEAD;
    const auto code = ErrorCode::Timeout;
    const char* message = "all fields test";
    const uint16_t svc_code = 65535;

    auto frame = ErrorSender::build_error_frame(msg_id, code, message, svc_code);
    ASSERT_GT(frame.size(), WireHeader::SIZE);

    WireHeader header{};
    auto* resp = parse_error_response(frame, &header);
    ASSERT_NE(resp, nullptr);

    // WireHeader round-trip
    EXPECT_EQ(header.msg_id, msg_id);
    EXPECT_EQ(header.flags, wire_flags::ERROR_RESPONSE);

    // ErrorResponse round-trip
    EXPECT_EQ(resp->code(), static_cast<uint16_t>(code));
    ASSERT_NE(resp->message(), nullptr);
    EXPECT_STREQ(resp->message()->c_str(), message);
    EXPECT_EQ(resp->service_error_code(), svc_code);
}
