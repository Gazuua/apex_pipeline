// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/adapter_interface.hpp>
#include <apex/core/adapter_state.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/adapter_error.hpp>
#include <apex/shared/adapters/cancellation_token.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace apex::shared::adapters
{

using apex::core::AdapterState;

/// 모든 어댑터의 공통 라이프사이클을 관리하는 CRTP 기본 클래스.
///
/// Derived가 구현해야 할 메서드:
///   void do_init(apex::core::CoreEngine& engine)
///   void do_drain()
///   void do_close()
///   std::string_view do_name() const noexcept
///
/// 선택적 override:
///   void do_close_per_core(uint32_t core_id)  — per-core 리소스 정리 (기본 no-op)
///
/// 소유 모델: Server에 전역 1개 인스턴스로 등록.
/// 코어별 리소스는 Derived 내부에서 CoreEngine::current_core_id()로 라우팅.
template <typename Derived> class AdapterBase
{
  public:
    AdapterBase() = default;
    ~AdapterBase() = default; // CRTP — virtual 불필요 (타입 소거는 AdapterWrapper가 담당)

    // Non-copyable, non-movable
    AdapterBase(const AdapterBase&) = delete;
    AdapterBase& operator=(const AdapterBase&) = delete;
    AdapterBase(AdapterBase&&) = delete;
    AdapterBase& operator=(AdapterBase&&) = delete;

    /// 어댑터 초기화. Server::run()에서 서비스 시작 전에 호출.
    void init(apex::core::CoreEngine& engine)
    {
        // Phase 1: 인프라 초기화 (파생 클래스보다 먼저)
        base_engine_ = &engine;
        tokens_.reserve(engine.core_count());
        for (uint32_t i = 0; i < engine.core_count(); ++i)
            tokens_.emplace_back();
        io_ctxs_.reserve(engine.core_count());
        for (uint32_t i = 0; i < engine.core_count(); ++i)
            io_ctxs_.push_back(&engine.io_context(i));
        // Phase 2: 파생 클래스 초기화
        // NOTE: state를 RUNNING으로 먼저 설정해야 do_init() 내에서 spawn_adapter_coro() 사용 가능.
        // 이 시점에서 다른 스레드가 is_ready()를 조회할 수 있으나, do_init()은 Server::run()
        // 내에서 단일 스레드로 실행되고, 코어 스레드는 아직 시작 전이므로 안전.
        state_.store(AdapterState::RUNNING, std::memory_order_release);
        static_cast<Derived*>(this)->do_init(engine);
    }

    /// 새 요청 거부 시그널 + 어댑터 코루틴 취소.
    void drain()
    {
        state_.store(AdapterState::DRAINING, std::memory_order_release);
        static_cast<Derived*>(this)->do_drain();
        cancel_all_coros(); // AdapterBase 강제 — 파생 클래스 오버라이드 불가
    }

    /// 리소스 정리. Phase 1: per-core cleanup, Phase 2: 전역 cleanup.
    void close()
    {
        state_.store(AdapterState::CLOSED, std::memory_order_release);
        // Phase 1: per-core cleanup (io_context에 post)
        if (!io_ctxs_.empty())
        {
            // shared_ptr: 타임아웃 후 close()가 반환해도 미실행 핸들러가 안전하게 접근 가능
            auto remaining = std::make_shared<std::atomic<uint32_t>>(static_cast<uint32_t>(io_ctxs_.size()));
            for (uint32_t i = 0; i < io_ctxs_.size(); ++i)
            {
                boost::asio::post(*io_ctxs_[i], [this, i, remaining] {
                    static_cast<Derived*>(this)->do_close_per_core(i);
                    remaining->fetch_sub(1, std::memory_order_release);
                });
            }
            // 타임아웃 방어: 5초 이내에 완료되지 않으면 경고 후 진행
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (remaining->load(std::memory_order_acquire) > 0)
            {
                if (std::chrono::steady_clock::now() > deadline)
                {
                    spdlog::warn("AdapterBase::close() timed out ({} cores remaining)",
                                 remaining->load(std::memory_order_relaxed));
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        // Phase 2: 전역 cleanup
        static_cast<Derived*>(this)->do_close();
    }

    /// [D2] Adapter-service 자동 배선 (기본 no-op).
    /// KafkaAdapter 등이 override하여 서비스 핸들러를 자동 감지.
    void wire_services(std::vector<std::unique_ptr<apex::core::ServiceBaseInterface>>&, apex::core::CoreEngine&) {}

    /// init 완료 + drain/close 안 됨
    [[nodiscard]] bool is_ready() const noexcept
    {
        return state_.load(std::memory_order_acquire) == AdapterState::RUNNING;
    }

    /// 현재 라이프사이클 상태
    [[nodiscard]] AdapterState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    /// 어댑터 이름
    [[nodiscard]] std::string_view name() const noexcept
    {
        return static_cast<const Derived*>(this)->do_name();
    }

    /// 전체 코어의 outstanding 어댑터 코루틴 합산. 폴링 루프에서 사용.
    [[nodiscard]] uint32_t outstanding_adapter_coros() const noexcept
    {
        uint32_t total = 0;
        for (const auto& t : tokens_)
            total += t.outstanding();
        return total;
    }

  protected:
    /// 지정된 코어에서 코루틴을 spawn하고 cancellation 추적에 등록.
    /// DRAINING/CLOSED 상태에서는 spawn 거부 (로그 경고, 코루틴 미실행).
    void spawn_adapter_coro(uint32_t core_id, boost::asio::awaitable<void> coro)
    {
        if (state_.load(std::memory_order_acquire) != AdapterState::RUNNING)
        {
            spdlog::warn("{}: spawn_adapter_coro rejected — adapter not RUNNING",
                         static_cast<Derived*>(this)->do_name());
            return;
        }
        auto& token = tokens_[core_id];
        auto slot = token.new_slot();
        boost::asio::co_spawn(
            *io_ctxs_[core_id],
            [&token, c = std::move(coro)]() mutable -> boost::asio::awaitable<void> {
                try
                {
                    co_await std::move(c);
                }
                catch (...)
                {}
                token.on_complete();
            },
            boost::asio::bind_cancellation_slot(slot, boost::asio::detached));
    }

    /// Per-core 리소스 정리 (기본 no-op). Derived에서 override 가능.
    void do_close_per_core([[maybe_unused]] uint32_t core_id) {}

    apex::core::CoreEngine* base_engine_{nullptr};

  private:
    void cancel_all_coros()
    {
        for (uint32_t i = 0; i < tokens_.size(); ++i)
        {
            boost::asio::post(*io_ctxs_[i], [this, i] { tokens_[i].cancel_all(); });
        }
    }

    std::atomic<AdapterState> state_{AdapterState::CLOSED};
    std::vector<CancellationToken> tokens_;
    std::vector<boost::asio::io_context*> io_ctxs_;
};

/// AdapterBase<Derived>를 AdapterInterface(apex_core)로 감싸는 래퍼.
/// Server::add_adapter<T>()에서 생성되어 타입 소거된 인터페이스로 저장된다.
template <typename Derived> class AdapterWrapper final : public apex::core::AdapterInterface
{
  public:
    template <typename... Args>
    explicit AdapterWrapper(Args&&... args)
        : adapter_(std::forward<Args>(args)...)
    {}

    void init(apex::core::CoreEngine& engine) override
    {
        adapter_.init(engine);
    }
    void drain() override
    {
        adapter_.drain();
    }
    void close() override
    {
        adapter_.close();
    }
    [[nodiscard]] bool is_ready() const noexcept override
    {
        return adapter_.is_ready();
    }
    [[nodiscard]] apex::core::AdapterState state() const noexcept override
    {
        return adapter_.state();
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return adapter_.name();
    }
    [[nodiscard]] uint32_t outstanding_adapter_coros() const noexcept override
    {
        return adapter_.outstanding_adapter_coros();
    }

    /// [D2] wire_services를 Derived 어댑터에 전달.
    void wire_services(std::vector<std::unique_ptr<apex::core::ServiceBaseInterface>>& services,
                       apex::core::CoreEngine& engine) override
    {
        adapter_.wire_services(services, engine);
    }

    Derived& get() noexcept
    {
        return adapter_;
    }
    const Derived& get() const noexcept
    {
        return adapter_;
    }

  private:
    Derived adapter_;
};

} // namespace apex::shared::adapters
