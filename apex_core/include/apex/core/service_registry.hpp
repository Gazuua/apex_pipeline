// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/service_base.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace apex::core
{

/// Per-core 타입 기반 서비스 레지스트리.
/// 단일 스레드 접근 전용 (per-core, 동기화 불필요).
class ServiceRegistry
{
  public:
    /// Server가 서비스 인스턴스 생성 시 호출. type_index로 자동 키잉.
    /// 소유권을 가져가므로 독립 사용(registry가 유일 소유자)에 적합.
    void register_service(std::unique_ptr<ServiceBaseInterface> svc)
    {
        auto key = std::type_index(typeid(*svc));
        map_[key] = svc.get();
        services_.push_back(std::move(svc));
    }

    /// 소유권 없이 서비스 참조만 등록. Server::run()에서 services 벡터와
    /// registry가 공존하는 현재 구조에서 사용. 서비스 수명은 호출자가 보장.
    void register_ref(ServiceBaseInterface& svc)
    {
        auto key = std::type_index(typeid(svc));
        map_[key] = &svc;
    }

    /// 타입으로 서비스 조회. 미등록 시 std::logic_error throw.
    template <typename T> T& get()
    {
        auto it = map_.find(std::type_index(typeid(T)));
        if (it == map_.end())
        {
            throw std::logic_error(std::string("ServiceRegistry::get<") + typeid(T).name() + ">: not registered");
        }
        return *static_cast<T*>(it->second);
    }

    /// 타입으로 서비스 탐색. 미등록 시 nullptr.
    /// @note 반환된 포인터는 레지스트리가 살아있는 동안 유효.
    ///       on_wire에서 받은 포인터를 멤버에 캐싱하는 것은 안전 (서비스 수명 = 레지스트리 수명).
    template <typename T> T* find()
    {
        auto it = map_.find(std::type_index(typeid(T)));
        if (it == map_.end())
            return nullptr;
        return static_cast<T*>(it->second);
    }

    /// 전체 서비스 순회.
    template <typename Fn> void for_each(Fn&& fn)
    {
        for (auto& svc : services_)
        {
            fn(*svc);
        }
    }

    [[nodiscard]] size_t size() const noexcept
    {
        return services_.size();
    }

  private:
    std::vector<std::unique_ptr<ServiceBaseInterface>> services_;
    std::unordered_map<std::type_index, ServiceBaseInterface*> map_;
};

/// 전 코어의 서비스 읽기 전용 뷰.
/// Phase 2(on_wire) 시점에만 사용. 모든 서비스 인스턴스가 생성 완료 상태이고
/// 코어 스레드 시작 전이므로 데이터 레이스 없이 안전.
class ServiceRegistryView
{
  public:
    explicit ServiceRegistryView(std::vector<ServiceRegistry*> registries)
        : registries_(std::move(registries))
    {}

    /// 전 코어의 특정 타입 서비스 순회 (읽기 전용).
    template <typename T> void for_each_core(std::function<void(uint32_t core_id, const T&)> fn) const
    {
        for (uint32_t i = 0; i < registries_.size(); ++i)
        {
            if (auto* svc = registries_[i]->find<T>())
            {
                fn(i, *svc);
            }
        }
    }

    /// 특정 코어의 서비스 조회 (읽기 전용). 미등록 시 std::logic_error.
    template <typename T> const T& get(uint32_t core_id) const
    {
        return registries_.at(core_id)->get<T>();
    }

  private:
    std::vector<ServiceRegistry*> registries_;
};

} // namespace apex::core
