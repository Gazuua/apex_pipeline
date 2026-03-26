// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/connection_handler.hpp>
#include <apex/core/connection_limiter.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_call.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/protocol.hpp>
#include <apex/core/scoped_logger.hpp>
#include <apex/core/session_manager.hpp>
#include <apex/core/tcp_acceptor.hpp>
#include <apex/core/transport.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
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

    /// Copy all handlers (default + msg_id handlers) from source to this listener's per-core dispatcher.
    /// Replaces sync_default_handler for full handler replication across multi-protocol listeners.
    virtual void sync_all_handlers(uint32_t core_id, const MessageDispatcher& source) = 0;

    /// Inject per-IP connection limiters. Called once before start(). No-op by default.
    virtual void inject_connection_limiters(std::vector<std::unique_ptr<ConnectionLimiter>>* /*limiters*/) {}
};

/// 프로토콜별 리스너. 포트 바인딩 + accept loop + per-core ConnectionHandler 관리.
template <Protocol P, Transport T = DefaultTransport> class Listener : public ListenerBase
{
  public:
    struct PerCoreHandler
    {
        MessageDispatcher dispatcher; // handler보다 먼저 선언 (소멸 순서 보장)
        ConnectionHandler<P, T> handler;

        PerCoreHandler(SessionManager& session_mgr, ConnectionHandlerConfig config, uint32_t core_id)
            : handler(session_mgr, dispatcher, config, core_id)
        {}
    };

    Listener(uint16_t port, typename P::Config protocol_config, typename T::Config transport_config, CoreEngine& engine,
             std::vector<SessionManager*> session_mgrs, ConnectionHandlerConfig handler_config,
             std::string bind_address = "0.0.0.0", uint32_t max_connections = 0, bool reuseport = false)
        : port_(port)
        , protocol_config_(std::move(protocol_config))
        , listener_state_(T::make_listener_state(transport_config))
        , engine_(engine)
        , bind_address_(std::move(bind_address))
        , max_connections_(max_connections)
        , reuseport_(reuseport)
    {
        // per-core handler 생성
        for (uint32_t i = 0; i < session_mgrs.size(); ++i)
        {
            per_core_handlers_.push_back(std::make_unique<PerCoreHandler>(*session_mgrs[i], handler_config, i));
        }
    }

    void start() override
    {
        [[maybe_unused]] uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());

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
                        if (max_connections_ > 0)
                        {
                            auto current = active_sessions();
                            if (current >= max_connections_)
                            {
                                logger_.warn("Connection rejected: max_connections limit ({}/{})", current,
                                             max_connections_);
                                boost::system::error_code ec;
                                socket.close(ec);
                                return;
                            }
                        }
                        dispatch_accept(std::move(socket), i, i);
                    },
                    bind_address_,
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
                [this](boost::asio::ip::tcp::socket socket) { on_accept(std::move(socket)); }, bind_address_));
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

    /// Graceful wind-down: 새 연결 수락 중단.
    /// 기존 세션은 자연 종료 대기 (Server의 coroutine drain이 담당).
    void drain() override
    {
        if (!started_.load(std::memory_order_acquire))
            return;
        for (auto& acc : acceptors_)
            acc->stop();
    }

    /// Immediate shutdown: 새 연결 수락 중단.
    /// 세션 강제 종료는 Server::finalize_shutdown()이 SessionManager를 통해 수행.
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

    /// Copy all handlers (default + msg_id handlers) from source dispatcher.
    void sync_all_handlers(uint32_t core_id, const MessageDispatcher& source) override
    {
        auto& target = per_core_handlers_[core_id]->dispatcher;

        // Sync default handler
        if (source.has_default_handler())
        {
            target.set_default_handler(source.default_handler());
        }

        // Sync all msg_id handlers
        for (const auto& [msg_id, handler] : source.handlers())
        {
            target.register_handler(msg_id, handler);
        }
    }

    /// Inject per-core ConnectionLimiters. Sets up release callbacks on ConnectionHandlers.
    /// Must be called before start(). Pointer must outlive this Listener.
    void inject_connection_limiters(std::vector<std::unique_ptr<ConnectionLimiter>>* limiters) override
    {
        connection_limiters_ = limiters;
        uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());
        for (uint32_t i = 0; i < num_cores; ++i)
        {
            per_core_handlers_[i]->handler.set_connection_closed_callback([this, i, num_cores](const std::string& ip) {
                if (!connection_limiters_ || ip.empty())
                    return;
                uint32_t owner = ConnectionLimiter::owner_core(ip, num_cores);
                auto* limiter = (*connection_limiters_)[owner].get();
                if (!limiter)
                    return; // Already destroyed during shutdown
                if (owner == i)
                {
                    limiter->decrement(ip);
                }
                else
                {
                    boost::asio::post(engine_.io_context(owner), [limiter, ip] { limiter->decrement(ip); });
                }
            });
        }
    }

  private:
    /// Extract client IP from raw socket (before Transport::wrap_socket).
    static std::string extract_ip(const boost::asio::ip::tcp::socket& socket)
    {
        boost::system::error_code ec;
        auto ep = socket.remote_endpoint(ec);
        return ec ? std::string{} : ep.address().to_string();
    }

    /// Common dispatch: extract IP → per-IP check → accept_connection.
    /// @param accepting_core the core where this callback runs (for co_spawn)
    /// @param target_core the core where the session will be handled
    void dispatch_accept(boost::asio::ip::tcp::socket socket, uint32_t accepting_core, uint32_t target_core)
    {
        auto ip = extract_ip(socket);

        if (connection_limiters_ && !ip.empty())
        {
            // Per-IP check requires coroutine for potential cross-core call
            boost::asio::co_spawn(engine_.io_context(accepting_core),
                                  checked_accept(std::move(socket), std::move(ip), accepting_core, target_core),
                                  boost::asio::detached);
        }
        else
        {
            // No per-IP limit — direct accept
            forward_to_handler(std::move(socket), std::move(ip), target_core);
        }
    }

    /// Wrap socket and post to target core's ConnectionHandler.
    void forward_to_handler(boost::asio::ip::tcp::socket socket, std::string remote_ip, uint32_t target_core)
    {
        auto wrapped = T::wrap_socket(std::move(socket), listener_state_);
        if (target_core == CoreEngine::current_core_id())
        {
            per_core_handlers_[target_core]->handler.accept_connection(
                std::move(wrapped), engine_.io_context(target_core), std::move(remote_ip));
        }
        else
        {
            auto& core_io = engine_.io_context(target_core);
            boost::asio::post(core_io,
                              [this, target_core, s = std::move(wrapped), ip = std::move(remote_ip)]() mutable {
                                  per_core_handlers_[target_core]->handler.accept_connection(
                                      std::move(s), engine_.io_context(target_core), std::move(ip));
                              });
        }
    }

    /// Coroutine: check per-IP limit (owner-shard pattern) then accept.
    boost::asio::awaitable<void> checked_accept(boost::asio::ip::tcp::socket socket, std::string remote_ip,
                                                uint32_t accepting_core, uint32_t target_core)
    {
        uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());
        uint32_t owner = ConnectionLimiter::owner_core(remote_ip, num_cores);

        bool allowed = false;
        if (owner == accepting_core)
        {
            // Same core — direct local check (no locking, no cross-core)
            allowed = (*connection_limiters_)[owner]->try_increment(remote_ip);
        }
        else
        {
            // Cross-core call to owner shard
            auto result = co_await cross_core_call(engine_, owner, [this, owner, ip = remote_ip] {
                return (*connection_limiters_)[owner]->try_increment(ip);
            });
            if (!result.has_value())
            {
                // Timeout/error: try_increment may or may not have executed on the owner core.
                // Post a compensating decrement — safe because decrement() handles unknown/zero-count IPs.
                logger_.warn("cross_core_call timeout for per-IP check (ip={})", remote_ip);
                auto* comp_limiter = (*connection_limiters_)[owner].get();
                if (comp_limiter)
                {
                    boost::asio::post(engine_.io_context(owner),
                                      [comp_limiter, ip = remote_ip] { comp_limiter->decrement(ip); });
                }
            }
            allowed = result.has_value() && *result;
        }

        if (!allowed)
        {
            logger_.info("Connection rejected: per-IP limit (ip={})", remote_ip);
            boost::system::error_code ec;
            socket.close(ec);
            co_return;
        }

        forward_to_handler(std::move(socket), std::move(remote_ip), target_core);
    }

    void on_accept(boost::asio::ip::tcp::socket socket)
    {
        if (max_connections_ > 0)
        {
            auto current = active_sessions();
            if (current >= max_connections_)
            {
                logger_.warn("Connection rejected: max_connections limit ({}/{})", current, max_connections_);
                boost::system::error_code ec;
                socket.close(ec);
                return;
            }
        }

        uint32_t num_cores = static_cast<uint32_t>(per_core_handlers_.size());
        uint32_t target_core = next_core_.fetch_add(1, std::memory_order_relaxed) % num_cores;

        // on_accept runs on core 0 (single-acceptor path)
        dispatch_accept(std::move(socket), 0, target_core);
    }

    ScopedLogger logger_{"Listener", ScopedLogger::NO_CORE};
    uint16_t port_;
    [[maybe_unused]] typename P::Config protocol_config_;
    typename T::ListenerState listener_state_;
    CoreEngine& engine_;
    std::string bind_address_;
    uint32_t max_connections_;
    bool reuseport_;
    std::vector<std::unique_ptr<PerCoreHandler>> per_core_handlers_;
    std::vector<std::unique_ptr<TcpAcceptor>> acceptors_;
    std::atomic<bool> started_{false}; // guards acceptors_ against concurrent access
    std::atomic<uint32_t> next_core_{0};
    std::vector<std::unique_ptr<ConnectionLimiter>>* connection_limiters_{nullptr};
};

} // namespace apex::core
