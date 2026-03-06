#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace apex::core {

enum class DispatchError : uint8_t {
    UnknownMessage,
    HandlerFailed,
};

/// O(1) message dispatcher using std::array indexed by msg_id (uint16_t).
/// Designed for per-service use. NOT thread-safe (per-core, single-thread).
class MessageDispatcher {
public:
    using Handler = std::function<void(uint16_t msg_id, std::span<const uint8_t> payload)>;

    MessageDispatcher() = default;

    void register_handler(uint16_t msg_id, Handler handler);
    void unregister_handler(uint16_t msg_id);

    [[nodiscard]] std::expected<void, DispatchError>
    dispatch(uint16_t msg_id, std::span<const uint8_t> payload) const;

    [[nodiscard]] bool has_handler(uint16_t msg_id) const noexcept;
    [[nodiscard]] size_t handler_count() const noexcept;

private:
    std::unique_ptr<std::array<Handler, 65536>> handlers_
        = std::make_unique<std::array<Handler, 65536>>();
    size_t handler_count_{0};
};

} // namespace apex::core
