// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/adapter_interface.hpp>
#include <apex/core/adapter_state.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/adapter_error.hpp>

#include <atomic>
#include <memory>
#include <string_view>
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
        static_cast<Derived*>(this)->do_init(engine);
        state_.store(AdapterState::RUNNING, std::memory_order_release);
    }

    /// 새 요청 거부 시그널. 진행 중 요청은 허용.
    void drain()
    {
        state_.store(AdapterState::DRAINING, std::memory_order_release);
        static_cast<Derived*>(this)->do_drain();
    }

    /// 리소스 정리. flush + 커넥션 해제.
    void close()
    {
        state_.store(AdapterState::CLOSED, std::memory_order_release);
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

  private:
    std::atomic<AdapterState> state_{AdapterState::CLOSED};
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
