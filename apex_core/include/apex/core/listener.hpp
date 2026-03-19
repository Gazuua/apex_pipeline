#pragma once

#include <apex/core/connection_handler.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/tcp_acceptor.hpp>
#include <apex/core/transport.hpp>

#include <boost/asio/post.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace apex::core
{

// Forward declaration
struct ConnectionHandlerConfig;

/// 의도적 virtual: CRTP 일관성 예외.
/// start/drain/stop은 서버 시작·종료 시 1회 호출로 비용 ≈ 0.
/// Hot path는 ConnectionHandler<P, T> 템플릿으로 zero-overhead.
class ListenerBase
{
  public:
    virtual ~ListenerBase() = default;
    // 현재 Server lifecycle은 동기 흐름. start()는 내부적으로 코루틴을
    // spawn하지만 자체가 코루틴일 필요 없음.
    virtual void start() = 0;
    virtual void drain() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual uint32_t active_sessions() const noexcept = 0;
    [[nodiscard]] virtual uint16_t port() const noexcept = 0;
    /// Per-core dispatcher 접근 (서비스 바인딩용).
    /// Listener<P>가 소유하는 per-core MessageDispatcher를 반환한다.
    [[nodiscard]] virtual MessageDispatcher& dispatcher(uint32_t core_id) = 0;

    /// Copy default handler from a shared dispatcher to this listener's per-core dispatcher.
    /// Used for multi-protocol: services register on the primary listener,
    /// and their default handler is propagated to secondary listeners.
    virtual void sync_default_handler(uint32_t core_id, const MessageDispatcher& source) = 0;
};

/// 프로토콜별 리스너. 포트 바인딩 + accept loop + per-core ConnectionHandler 관리.
template <Protocol P, Transport T = DefaultTransport> class Listener : public ListenerBase
{
  public:
    struct PerCoreHandler
    {
        MessageDispatcher dispatcher; // handler보다 먼저 선언 (소멸 순서 보장)
        ConnectionHandler<P, T> handler;

        PerCoreHandler(SessionManager& session_mgr, ConnectionHandlerConfig config)
            : handler(session_mgr, dispatcher, config)
        {}
    };

    Listener(uint16_t port, typename P::Config protocol_config, CoreEngine& engine,
             std::vector<SessionManager*> session_mgrs, ConnectionHandlerConfig handler_config, bool reuseport = false)
        : port_(port)
        , protocol_config_(std::move(protocol_config))
        , engine_(engine)
        , reuseport_(reuseport)
    {
        // per-core handler 생성
        for (auto* mgr : session_mgrs)
        {
            per_core_handlers_.push_back(std::make_unique<PerCoreHandler>(*mgr, handler_config));
        }
    }

    void start() override
    {
        uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());

        // Build acceptors into a local vector first, then move-assign to the
        // member in one shot.  This ensures acceptors_ is either empty or
        // fully populated — no intermediate push_back states visible to
        // concurrent drain()/stop()/port() calls.
        std::vector<std::unique_ptr<TcpAcceptor>> local;

#if !defined(_WIN32)
        if (reuseport_)
        {
            // Linux: per-core acceptor with SO_REUSEPORT
            for (uint32_t i = 0; i < num_cores; ++i)
            {
                auto& core_io = engine_.io_context(i);
                local.push_back(std::make_unique<TcpAcceptor>(
                    core_io, port_,
                    [this, i](boost::asio::ip::tcp::socket socket) {
                        per_core_handlers_[i]->handler.accept_connection(std::move(socket), engine_.io_context(i));
                    },
                    boost::asio::ip::tcp::v4(),
                    /*reuseport=*/true));
            }
        }
        else
#endif
        {
            // Single acceptor, round-robin distribution
            // Accept on core 0's io_context (or a control io_context)
            local.push_back(std::make_unique<TcpAcceptor>(
                engine_.io_context(0), port_,
                [this](boost::asio::ip::tcp::socket socket) { on_accept(std::move(socket)); }));
        }

        // Publish fully-built vector to the member.
        acceptors_ = std::move(local);

        for (auto& acc : acceptors_)
            acc->start();

        // Signal that acceptors_ is fully built and started.
        // drain()/stop()/port() acquire-load this flag to establish
        // happens-before with the writes above, preventing data-race
        // on the vector.
        started_.store(true, std::memory_order_release);
    }

    void drain() override
    {
        if (!started_.load(std::memory_order_acquire))
            return;
        for (auto& acc : acceptors_)
            acc->stop();
    }

    void stop() override
    {
        if (!started_.load(std::memory_order_acquire))
            return;
        for (auto& acc : acceptors_)
            acc->stop();
    }

    [[nodiscard]] uint32_t active_sessions() const noexcept override
    {
        uint32_t total = 0;
        for (const auto& pch : per_core_handlers_)
        {
            total += pch->handler.active_sessions();
        }
        return total;
    }

    [[nodiscard]] uint16_t port() const noexcept override
    {
        if (!started_.load(std::memory_order_acquire))
            return port_;
        return acceptors_.empty() ? port_ : acceptors_[0]->port();
    }

    /// Listener의 per-core dispatcher에 접근 (서비스 바인딩용)
    [[nodiscard]] MessageDispatcher& dispatcher(uint32_t core_id) override
    {
        return per_core_handlers_[core_id]->dispatcher;
    }

    /// Copy default handler from source dispatcher to this listener's dispatcher.
    void sync_default_handler(uint32_t core_id, const MessageDispatcher& source) override
    {
        if (source.has_default_handler())
        {
            per_core_handlers_[core_id]->dispatcher.set_default_handler(source.default_handler());
        }
    }

  private:
    void on_accept(boost::asio::ip::tcp::socket socket)
    {
        uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());
        uint32_t core_id = next_core_.fetch_add(1, std::memory_order_relaxed) % num_cores;

        auto& core_io = engine_.io_context(core_id);
        boost::asio::post(core_io, [this, core_id, s = std::move(socket)]() mutable {
            per_core_handlers_[core_id]->handler.accept_connection(std::move(s), engine_.io_context(core_id));
        });
    }

    uint16_t port_;
    typename P::Config protocol_config_;
    CoreEngine& engine_;
    bool reuseport_;
    std::vector<std::unique_ptr<PerCoreHandler>> per_core_handlers_;
    std::vector<std::unique_ptr<TcpAcceptor>> acceptors_;
    std::atomic<bool> started_{false}; // guards acceptors_ against concurrent access
    std::atomic<uint32_t> next_core_{0};
};

} // namespace apex::core
