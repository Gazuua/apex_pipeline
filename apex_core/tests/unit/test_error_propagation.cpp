#include <apex/core/error_code.hpp>
#include <apex/core/error_sender.hpp>
#include <apex/core/result.hpp>
#include <apex/core/wire_header.hpp>

#include <generated/error_response_generated.h>
#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(Result, OkValue) {
    Result<int> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(Result, ErrorValue) {
    Result<int> r = error(ErrorCode::Timeout);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::Timeout);
}

TEST(Result, VoidOk) {
    Result<void> r = ok();
    EXPECT_TRUE(r.has_value());
}

TEST(Result, VoidError) {
    Result<void> r = error(ErrorCode::SessionClosed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::SessionClosed);
}

TEST(ErrorCode, NameLookup) {
    EXPECT_EQ(error_code_name(ErrorCode::Ok), "Ok");
    EXPECT_EQ(error_code_name(ErrorCode::Timeout), "Timeout");
    EXPECT_EQ(error_code_name(ErrorCode::HandlerNotFound), "HandlerNotFound");
}

TEST(ErrorSender, BuildErrorFrame) {
    auto frame = ErrorSender::build_error_frame(
        0x0042, ErrorCode::Timeout, "request timed out");

    ASSERT_GT(frame.size(), WireHeader::SIZE);

    auto header = WireHeader::parse(frame);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->msg_id, 0x0042);
    EXPECT_TRUE(header->flags & wire_flags::ERROR_RESPONSE);

    auto payload = std::span<const uint8_t>(
        frame.data() + WireHeader::SIZE, header->body_size);
    auto resp = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        payload.data());
    EXPECT_EQ(resp->code(), static_cast<uint16_t>(ErrorCode::Timeout));
    EXPECT_STREQ(resp->message()->c_str(), "request timed out");
}

TEST(ErrorCode, HandlerExceptionName) {
    EXPECT_EQ(error_code_name(ErrorCode::HandlerException), "HandlerException");
}

TEST(ErrorCode, SendFailedName) {
    EXPECT_EQ(error_code_name(ErrorCode::SendFailed), "SendFailed");
}

TEST(Result, HandlerExceptionError) {
    Result<void> r = error(ErrorCode::HandlerException);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::HandlerException);
}

TEST(Result, SendFailedError) {
    Result<void> r = error(ErrorCode::SendFailed);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ErrorCode::SendFailed);
}

TEST(ErrorSender, BuildErrorFrameNoMessage) {
    auto frame = ErrorSender::build_error_frame(
        0x0001, ErrorCode::HandlerNotFound);

    auto header = WireHeader::parse(frame);
    ASSERT_TRUE(header.has_value());
    EXPECT_TRUE(header->flags & wire_flags::ERROR_RESPONSE);

    auto payload = std::span<const uint8_t>(
        frame.data() + WireHeader::SIZE, header->body_size);
    auto resp = flatbuffers::GetRoot<apex::messages::ErrorResponse>(
        payload.data());
    EXPECT_EQ(resp->code(),
              static_cast<uint16_t>(ErrorCode::HandlerNotFound));
}
