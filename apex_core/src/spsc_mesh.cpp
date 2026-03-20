// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_mesh.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_dispatcher.hpp>

#include <spdlog/spdlog.h>

#include <cassert>

namespace apex::core
{

SpscMesh::SpscMesh(uint32_t num_cores, size_t queue_capacity,
                   const std::vector<boost::asio::io_context*>& core_io_contexts)
    : num_cores_(num_cores)
{
    assert(core_io_contexts.size() == num_cores);
    queues_.resize(static_cast<size_t>(num_cores) * num_cores);
    for (uint32_t src = 0; src < num_cores; ++src)
    {
        for (uint32_t dst = 0; dst < num_cores; ++dst)
        {
            if (src == dst)
                continue;
            queues_[static_cast<size_t>(src) * num_cores + dst] =
                std::make_unique<SpscQueue<CoreMessage>>(queue_capacity, *core_io_contexts[src]);
        }
    }
}

SpscMesh::~SpscMesh() = default;

SpscQueue<CoreMessage>& SpscMesh::queue(uint32_t src, uint32_t dst)
{
    assert(src < num_cores_ && dst < num_cores_ && "queue index out of range");
    assert(src != dst && "cannot send to self via SPSC mesh");
    auto& q = queues_[static_cast<size_t>(src) * num_cores_ + dst];
    assert(q && "queue must exist for src != dst");
    return *q;
}

size_t SpscMesh::drain_all_for(uint32_t dst_core, const CrossCoreDispatcher& dispatcher,
                               const std::function<void(uint32_t, const CoreMessage&)>& legacy_handler,
                               size_t batch_limit)
{
    size_t total = 0;

    for (uint32_t src = 0; src < num_cores_; ++src)
    {
        if (src == dst_core)
            continue;
        if (total >= batch_limit)
            break;

        auto& q = queue(src, dst_core);
        while (total < batch_limit)
        {
            auto msg = q.try_dequeue();
            if (!msg)
                break;

            if (msg->op == CrossCoreOp::LegacyCrossCoreFn)
            {
                auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                if (task)
                {
                    try
                    {
                        (*task)();
                    }
                    catch (const std::exception& e)
                    {
                        spdlog::error("Core {} cross-core task exception: {}", dst_core, e.what());
                    }
                    catch (...)
                    {
                        spdlog::error("Core {} cross-core task unknown exception", dst_core);
                    }
                    delete task;
                }
            }
            else if (dispatcher.has_handler(msg->op))
            {
                dispatcher.dispatch(dst_core, msg->source_core, msg->op, reinterpret_cast<void*>(msg->data));
            }
            else if (legacy_handler)
            {
                legacy_handler(dst_core, *msg);
            }
            ++total;
        }

        // Notify waiting producer for this queue
        q.notify_producer_if_waiting();
    }

    return total;
}

void SpscMesh::shutdown()
{
    for (uint32_t src = 0; src < num_cores_; ++src)
    {
        for (uint32_t dst = 0; dst < num_cores_; ++dst)
        {
            if (src == dst)
                continue;
            auto& q = queue(src, dst);

            q.cancel_waiting_producer();

            // Drain remaining — clean up LegacyCrossCoreFn heap pointers
            while (auto msg = q.try_dequeue())
            {
                if (msg->op == CrossCoreOp::LegacyCrossCoreFn)
                {
                    auto* task = reinterpret_cast<std::function<void()>*>(msg->data);
                    delete task;
                }
            }
        }
    }
}

} // namespace apex::core
