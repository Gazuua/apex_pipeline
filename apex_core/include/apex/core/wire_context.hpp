// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::core
{

// 순환 의존 방지를 위한 전방 선언
class Server;
class ServiceRegistry;
class ServiceRegistryView;
class PeriodicTaskScheduler;

/// Phase 2 Context: 서비스 간 와이어링 + 유틸리티.
struct WireContext
{
    Server& server;
    uint32_t core_id;
    ServiceRegistry& local_registry;
    ServiceRegistryView& global_registry;
    PeriodicTaskScheduler& scheduler;
};

} // namespace apex::core
