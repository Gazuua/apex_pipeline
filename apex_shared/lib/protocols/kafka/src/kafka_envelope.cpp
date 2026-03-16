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

} // namespace apex::shared::protocols::kafka
