// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

// NOTE: core_engine.hpp를 포함하지 않는다 (순환 include 방지).
// io_context 포인터를 직접 받아서 CoreContext 의존성 제거.

#include <apex/core/core_message.hpp>
#include <apex/core/cross_core_op.hpp>
#include <apex/core/spsc_queue.hpp>

#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace apex::core
{

// Forward declaration
class CrossCoreDispatcher;

/// N×(N-1) SPSC all-to-all mesh for inter-core communication.
/// Each core pair (src, dst) has a dedicated SpscQueue.
/// src == dst is disallowed (nullptr slot).
class SpscMesh
{
  public:
    /// @param num_cores Number of cores in the mesh.
    /// @param queue_capacity Per-queue slot count (power-of-2).
    /// @param core_io_contexts Each core's io_context pointer (producer binding).
    SpscMesh(uint32_t num_cores, size_t queue_capacity, const std::vector<boost::asio::io_context*>& core_io_contexts);

    ~SpscMesh();

    SpscMesh(const SpscMesh&) = delete;
    SpscMesh& operator=(const SpscMesh&) = delete;

    /// Access the SPSC queue from src to dst. src != dst required (assert).
    [[nodiscard]] SpscQueue<CoreMessage>& queue(uint32_t src, uint32_t dst);

    /// Drain all incoming queues for dst_core. Dispatches messages via dispatcher.
    /// Also notifies waiting producers after drain.
    /// @return Total messages processed.
    size_t drain_all_for(uint32_t dst_core, const CrossCoreDispatcher& dispatcher,
                         const std::function<void(uint32_t, const CoreMessage&)>& legacy_handler, size_t batch_limit);

    /// Shutdown: cancel all waiting producers, drain remaining LegacyCrossCoreFn.
    void shutdown();

    [[nodiscard]] uint32_t core_count() const noexcept
    {
        return num_cores_;
    }

  private:
    uint32_t num_cores_;
    std::vector<std::unique_ptr<SpscQueue<CoreMessage>>> queues_; // [src * N + dst]
};

} // namespace apex::core
