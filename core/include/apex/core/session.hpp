#pragma once

#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>

namespace apex::core {

/// 고유 세션 식별자 (코어별 단조 증가)
using SessionId = uint64_t;

/// 단일 클라이언트 연결을 나타내는 클래스.
/// SessionManager가 shared_ptr로 소유 (코루틴 안전성 보장).
///
/// 생명주기: Connected -> Active -> Closed
class Session : public std::enable_shared_from_this<Session> {
public:
    enum class State : uint8_t {
        Connected,
        Active,
        // Closing 제거 (I-2): 미사용 상태였음. graceful shutdown은 Closed로 직접 전이.
        Closed,
    };

    Session(SessionId id, boost::asio::ip::tcp::socket socket,
            uint32_t core_id, size_t recv_buf_capacity = 8192);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// 프레임 응답을 이 세션에 비동기 전송.
    /// @pre 호출자는 이 세션이 속한 io_context의 implicit strand에서 호출해야 한다.
    [[nodiscard]] boost::asio::awaitable<bool>
    async_send(const WireHeader& header, std::span<const uint8_t> payload);

    /// 미리 빌드된 로우 프레임 비동기 전송.
    [[nodiscard]] boost::asio::awaitable<bool>
    async_send_raw(std::span<const uint8_t> data);

    /// 세션 그레이스풀 종료.
    void close();

    [[nodiscard]] SessionId id() const noexcept { return id_; }
    [[nodiscard]] uint32_t core_id() const noexcept { return core_id_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool is_open() const noexcept {
        return state_ == State::Connected || state_ == State::Active;
    }
    [[nodiscard]] boost::asio::ip::tcp::socket& socket() noexcept {
        return socket_;
    }
    [[nodiscard]] RingBuffer& recv_buffer() noexcept { return recv_buf_; }

    void set_state(State s) noexcept { state_ = s; }

private:
    SessionId id_;
    uint32_t core_id_;
    State state_{State::Connected};
    boost::asio::ip::tcp::socket socket_;
    RingBuffer recv_buf_;
};

using SessionPtr = std::shared_ptr<Session>;

} // namespace apex::core
