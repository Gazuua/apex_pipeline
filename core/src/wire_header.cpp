#include <apex/core/wire_header.hpp>

#ifdef _MSC_VER
#include <cstdlib>  // _byteswap_ushort, _byteswap_ulong
#endif

namespace apex::core {
namespace {

#ifdef _MSC_VER
inline uint16_t hton16(uint16_t v) { return _byteswap_ushort(v); }
inline uint32_t hton32(uint32_t v) { return _byteswap_ulong(v); }
#else
inline uint16_t hton16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t hton32(uint32_t v) { return __builtin_bswap32(v); }
#endif
inline uint16_t ntoh16(uint16_t v) { return hton16(v); }
inline uint32_t ntoh32(uint32_t v) { return hton32(v); }

} // anonymous namespace

std::expected<WireHeader, ParseError>
WireHeader::parse(std::span<const uint8_t> data) {
    if (data.size() < SIZE) {
        return std::unexpected(ParseError::InsufficientData);
    }

    WireHeader h;
    h.version = data[0];

    uint16_t raw16;
    uint32_t raw32;

    std::memcpy(&raw16, data.data() + 1, sizeof(uint16_t));
    h.msg_id = ntoh16(raw16);

    std::memcpy(&raw32, data.data() + 3, sizeof(uint32_t));
    h.body_size = ntoh32(raw32);

    std::memcpy(&raw16, data.data() + 7, sizeof(uint16_t));
    h.flags = ntoh16(raw16);

    h.reserved = data[9];

    if (h.body_size > MAX_BODY_SIZE) {
        return std::unexpected(ParseError::BodyTooLarge);
    }

    return h;
}

void WireHeader::serialize(std::span<uint8_t, SIZE> out) const {
    out[0] = version;

    uint16_t net16 = hton16(msg_id);
    std::memcpy(out.data() + 1, &net16, sizeof(uint16_t));

    uint32_t net32 = hton32(body_size);
    std::memcpy(out.data() + 3, &net32, sizeof(uint32_t));

    net16 = hton16(flags);
    std::memcpy(out.data() + 7, &net16, sizeof(uint16_t));

    out[9] = reserved;
}

std::array<uint8_t, WireHeader::SIZE> WireHeader::serialize() const {
    std::array<uint8_t, SIZE> buf{};
    serialize(std::span<uint8_t, SIZE>{buf});
    return buf;
}

} // namespace apex::core
