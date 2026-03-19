#include <apex/shared/protocols/kafka/envelope_builder.hpp>

#include <chrono>
#include <cstring>
#include <limits>

namespace apex::shared::protocols::kafka
{

// ============================================================
// 빌더 체인 메서드
// ============================================================

EnvelopeBuilder& EnvelopeBuilder::routing(uint32_t msg_id, uint16_t flags)
{
    msg_id_ = msg_id;
    flags_ = flags;
    return *this;
}

EnvelopeBuilder& EnvelopeBuilder::metadata(uint16_t core_id, uint64_t corr_id, uint16_t source_id, uint64_t session_id,
                                           uint64_t user_id)
{
    core_id_ = core_id;
    corr_id_ = corr_id;
    source_id_ = source_id;
    session_id_ = session_id;
    user_id_ = user_id;
    return *this;
}

EnvelopeBuilder& EnvelopeBuilder::reply_topic(std::string_view topic)
{
    reply_topic_.assign(topic.data(), topic.size());
    return *this;
}

EnvelopeBuilder& EnvelopeBuilder::payload(std::span<const uint8_t> data)
{
    payload_ = data;
    return *this;
}

// ============================================================
// 내부 유틸 — 크기 계산 및 in-place 직렬화
// ============================================================

size_t EnvelopeBuilder::total_size() const noexcept
{
    size_t size = ENVELOPE_HEADER_SIZE; // RoutingHeader(8) + MetadataPrefix(40) = 48

    if (!reply_topic_.empty() && reply_topic_.size() <= std::numeric_limits<uint16_t>::max())
    {
        size += sizeof(uint16_t) + reply_topic_.size(); // ReplyTopicHeader
    }

    size += payload_.size();
    return size;
}

void EnvelopeBuilder::serialize_into(uint8_t* buf) const
{
    // --- RoutingHeader 구성 ---
    // HAS_REPLY_TOPIC 플래그: reply_topic_ 유무에 따라 자동 설정/해제
    RoutingHeader rh;
    rh.msg_id = msg_id_;
    rh.flags = flags_;
    if (!reply_topic_.empty() && reply_topic_.size() <= std::numeric_limits<uint16_t>::max())
    {
        rh.flags |= routing_flags::HAS_REPLY_TOPIC;
    }
    else
    {
        rh.flags &= static_cast<uint16_t>(~routing_flags::HAS_REPLY_TOPIC);
    }

    // --- MetadataPrefix 구성 ---
    MetadataPrefix mp;
    mp.core_id = core_id_;
    mp.corr_id = corr_id_;
    mp.source_id = source_id_;
    mp.session_id = session_id_;
    mp.user_id = user_id_;
    // timestamp: 현재 시각(epoch milliseconds)으로 자동 설정
    mp.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    // --- 직렬화 ---
    size_t offset = 0;

    // RoutingHeader (8바이트)
    rh.serialize(std::span<uint8_t, RoutingHeader::SIZE>{buf + offset, RoutingHeader::SIZE});
    offset += RoutingHeader::SIZE;

    // MetadataPrefix (40바이트)
    mp.serialize(std::span<uint8_t, MetadataPrefix::SIZE>{buf + offset, MetadataPrefix::SIZE});
    offset += MetadataPrefix::SIZE;

    // ReplyTopicHeader (옵션: 2 + N바이트)
    if (!reply_topic_.empty() && reply_topic_.size() <= std::numeric_limits<uint16_t>::max())
    {
        auto len = static_cast<uint16_t>(reply_topic_.size());

#ifdef _MSC_VER
        uint16_t net_len = _byteswap_ushort(len);
#else
        uint16_t net_len = __builtin_bswap16(len);
#endif
        std::memcpy(buf + offset, &net_len, sizeof(uint16_t));
        offset += sizeof(uint16_t);

        std::memcpy(buf + offset, reply_topic_.data(), len);
        offset += len;
    }

    // Payload
    if (!payload_.empty())
    {
        std::memcpy(buf + offset, payload_.data(), payload_.size());
    }
}

// ============================================================
// 빌드 메서드
// ============================================================

std::span<uint8_t> EnvelopeBuilder::build_into(apex::core::BumpAllocator& alloc)
{
    const size_t size = total_size();
    // align=1: 바이트 배열은 정렬 제약 없음
    void* mem = alloc.allocate(size, 1);
    if (!mem)
    {
        return {};
    }

    auto* buf = static_cast<uint8_t*>(mem);
    serialize_into(buf);
    return {buf, size};
}

std::vector<uint8_t> EnvelopeBuilder::build()
{
    const size_t size = total_size();
    std::vector<uint8_t> buf(size);
    serialize_into(buf.data());
    return buf;
}

} // namespace apex::shared::protocols::kafka
