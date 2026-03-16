#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <bit>

#ifdef _MSC_VER
#include <cstdlib>
#endif

namespace apex::shared::protocols::kafka {

static_assert(std::endian::native == std::endian::little,
              "KafkaEnvelope assumes little-endian host");

namespace {
#ifdef _MSC_VER
inline uint16_t hton16(uint16_t v) { return _byteswap_ushort(v); }
inline uint32_t hton32(uint32_t v) { return _byteswap_ulong(v); }
inline uint64_t hton64(uint64_t v) { return _byteswap_uint64(v); }
#else
inline uint16_t hton16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t hton32(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t hton64(uint64_t v) { return __builtin_bswap64(v); }
#endif
inline uint16_t ntoh16(uint16_t v) { return hton16(v); }
inline uint32_t ntoh32(uint32_t v) { return hton32(v); }
inline uint64_t ntoh64(uint64_t v) { return hton64(v); }
} // anonymous namespace

// --- RoutingHeader ---

std::expected<RoutingHeader, EnvelopeError>
RoutingHeader::parse(std::span<const uint8_t> data) {
    if (data.size() < SIZE) {
        return std::unexpected(EnvelopeError::InsufficientData);
    }

    RoutingHeader h;
    uint16_t raw16;
    uint32_t raw32;

    std::memcpy(&raw16, data.data() + OFF_HEADER_VERSION, sizeof(uint16_t));
    h.header_version = ntoh16(raw16);

    if (h.header_version != CURRENT_VERSION) {
        return std::unexpected(EnvelopeError::UnsupportedVersion);
    }

    std::memcpy(&raw16, data.data() + OFF_FLAGS, sizeof(uint16_t));
    h.flags = ntoh16(raw16);

    std::memcpy(&raw32, data.data() + OFF_MSG_ID, sizeof(uint32_t));
    h.msg_id = ntoh32(raw32);

    return h;
}

void RoutingHeader::serialize(std::span<uint8_t, SIZE> out) const {
    uint16_t net16 = hton16(header_version);
    std::memcpy(out.data() + OFF_HEADER_VERSION, &net16, sizeof(uint16_t));

    net16 = hton16(flags);
    std::memcpy(out.data() + OFF_FLAGS, &net16, sizeof(uint16_t));

    uint32_t net32 = hton32(msg_id);
    std::memcpy(out.data() + OFF_MSG_ID, &net32, sizeof(uint32_t));
}

std::array<uint8_t, RoutingHeader::SIZE> RoutingHeader::serialize() const {
    std::array<uint8_t, SIZE> buf{};
    serialize(std::span<uint8_t, SIZE>{buf});
    return buf;
}

// --- MetadataPrefix ---

std::expected<MetadataPrefix, EnvelopeError>
MetadataPrefix::parse(std::span<const uint8_t> data) {
    if (data.size() < SIZE) {
        return std::unexpected(EnvelopeError::InsufficientData);
    }

    MetadataPrefix m;
    uint16_t raw16;
    uint32_t raw32;
    uint64_t raw64;

    std::memcpy(&raw32, data.data() + OFF_META_VERSION, sizeof(uint32_t));
    m.meta_version = ntoh32(raw32);

    if (m.meta_version != CURRENT_VERSION) {
        return std::unexpected(EnvelopeError::UnsupportedVersion);
    }

    std::memcpy(&raw16, data.data() + OFF_CORE_ID, sizeof(uint16_t));
    m.core_id = ntoh16(raw16);

    std::memcpy(&raw64, data.data() + OFF_CORR_ID, sizeof(uint64_t));
    m.corr_id = ntoh64(raw64);

    std::memcpy(&raw16, data.data() + OFF_SOURCE_ID, sizeof(uint16_t));
    m.source_id = ntoh16(raw16);

    std::memcpy(&raw64, data.data() + OFF_SESSION_ID, sizeof(uint64_t));
    m.session_id = ntoh64(raw64);

    std::memcpy(&raw64, data.data() + OFF_USER_ID, sizeof(uint64_t));
    m.user_id = ntoh64(raw64);

    std::memcpy(&raw64, data.data() + OFF_TIMESTAMP, sizeof(uint64_t));
    m.timestamp = ntoh64(raw64);

    return m;
}

void MetadataPrefix::serialize(std::span<uint8_t, SIZE> out) const {
    uint32_t net32 = hton32(meta_version);
    std::memcpy(out.data() + OFF_META_VERSION, &net32, sizeof(uint32_t));

    uint16_t net16 = hton16(core_id);
    std::memcpy(out.data() + OFF_CORE_ID, &net16, sizeof(uint16_t));

    uint64_t net64 = hton64(corr_id);
    std::memcpy(out.data() + OFF_CORR_ID, &net64, sizeof(uint64_t));

    net16 = hton16(source_id);
    std::memcpy(out.data() + OFF_SOURCE_ID, &net16, sizeof(uint16_t));

    net64 = hton64(session_id);
    std::memcpy(out.data() + OFF_SESSION_ID, &net64, sizeof(uint64_t));

    net64 = hton64(user_id);
    std::memcpy(out.data() + OFF_USER_ID, &net64, sizeof(uint64_t));

    net64 = hton64(timestamp);
    std::memcpy(out.data() + OFF_TIMESTAMP, &net64, sizeof(uint64_t));
}

std::array<uint8_t, MetadataPrefix::SIZE> MetadataPrefix::serialize() const {
    std::array<uint8_t, SIZE> buf{};
    serialize(std::span<uint8_t, SIZE>{buf});
    return buf;
}

// --- ReplyTopicHeader ---

std::vector<uint8_t>
ReplyTopicHeader::serialize(std::string_view reply_topic) {
    if (reply_topic.empty()) {
        return {};
    }

    auto len = static_cast<uint16_t>(reply_topic.size());
    std::vector<uint8_t> buf(sizeof(uint16_t) + len);

    uint16_t net_len = hton16(len);
    std::memcpy(buf.data(), &net_len, sizeof(uint16_t));
    std::memcpy(buf.data() + sizeof(uint16_t), reply_topic.data(), len);

    return buf;
}

std::expected<std::pair<std::string, size_t>, EnvelopeError>
ReplyTopicHeader::parse(std::span<const uint8_t> data) {
    if (data.size() < sizeof(uint16_t)) {
        return std::unexpected(EnvelopeError::InsufficientData);
    }

    uint16_t raw_len;
    std::memcpy(&raw_len, data.data(), sizeof(uint16_t));
    uint16_t len = ntoh16(raw_len);

    size_t total = sizeof(uint16_t) + len;
    if (data.size() < total) {
        return std::unexpected(EnvelopeError::InsufficientData);
    }

    std::string topic(reinterpret_cast<const char*>(data.data() + sizeof(uint16_t)), len);
    return std::pair{std::move(topic), total};
}

// --- Envelope helpers ---

std::vector<uint8_t> build_full_envelope(
    const RoutingHeader& routing,
    const MetadataPrefix& metadata,
    std::string_view reply_topic,
    std::span<const uint8_t> payload)
{
    auto routing_bytes = routing.serialize();
    auto metadata_bytes = metadata.serialize();
    auto reply_bytes = ReplyTopicHeader::serialize(reply_topic);

    std::vector<uint8_t> buf;
    buf.reserve(routing_bytes.size() + metadata_bytes.size()
                + reply_bytes.size() + payload.size());

    buf.insert(buf.end(), routing_bytes.begin(), routing_bytes.end());
    buf.insert(buf.end(), metadata_bytes.begin(), metadata_bytes.end());
    if (!reply_bytes.empty()) {
        buf.insert(buf.end(), reply_bytes.begin(), reply_bytes.end());
    }
    buf.insert(buf.end(), payload.begin(), payload.end());

    return buf;
}

size_t envelope_payload_offset(
    uint16_t flags,
    std::span<const uint8_t> data)
{
    size_t offset = ENVELOPE_HEADER_SIZE;

    if (flags & routing_flags::HAS_REPLY_TOPIC) {
        if (data.size() > offset + sizeof(uint16_t)) {
            auto result = ReplyTopicHeader::parse(data.subspan(offset));
            if (result.has_value()) {
                offset += result->second;
            }
        }
    }

    return offset;
}

std::string extract_reply_topic(
    uint16_t flags,
    std::span<const uint8_t> data)
{
    if (!(flags & routing_flags::HAS_REPLY_TOPIC)) {
        return {};
    }

    if (data.size() <= ENVELOPE_HEADER_SIZE + sizeof(uint16_t)) {
        return {};
    }

    auto result = ReplyTopicHeader::parse(data.subspan(ENVELOPE_HEADER_SIZE));
    if (!result.has_value()) {
        return {};
    }

    return std::move(result->first);
}

} // namespace apex::shared::protocols::kafka
