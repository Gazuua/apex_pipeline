#include <apex/core/error_sender.hpp>

#include <generated/error_response_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <cstring>

namespace apex::core {

std::vector<uint8_t> ErrorSender::build_error_frame(
    uint16_t original_msg_id,
    ErrorCode code,
    std::string_view message)
{
    flatbuffers::FlatBufferBuilder builder(128);

    // I-3: 항상 CreateString 호출 — 빈 문자열도 유효한 FlatBuffers String 생성.
    // 조건 분기 시 기본 초기화(0) Offset → 수신측 nullptr deref 위험.
    auto msg_offset = builder.CreateString(message.data(), message.size());

    auto resp = apex::messages::CreateErrorResponse(
        builder,
        static_cast<uint16_t>(code),
        msg_offset);
    builder.Finish(resp);

    auto payload_data = builder.GetBufferPointer();
    auto payload_size = builder.GetSize();

    WireHeader header{
        .version = WireHeader::CURRENT_VERSION,
        .msg_id = original_msg_id,
        .body_size = static_cast<uint32_t>(payload_size),
        .flags = wire_flags::ERROR_RESPONSE,
    };

    std::vector<uint8_t> frame(header.frame_size());
    auto hdr_bytes = header.serialize();
    std::memcpy(frame.data(), hdr_bytes.data(), WireHeader::SIZE);
    std::memcpy(frame.data() + WireHeader::SIZE, payload_data, payload_size);

    return frame;
}

} // namespace apex::core
