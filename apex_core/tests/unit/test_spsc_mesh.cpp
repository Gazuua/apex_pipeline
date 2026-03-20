// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/spsc_mesh.hpp>
#include <apex/core/core_engine.hpp>
#include <apex/core/cross_core_dispatcher.hpp>

#include <gtest/gtest.h>

#include <atomic>

namespace apex::core
{

class SpscMeshTest : public ::testing::Test
{
  protected:
    static constexpr uint32_t NUM_CORES = 4;
    static constexpr size_t QUEUE_CAPACITY = 64;

    void SetUp() override
    {
        for (uint32_t i = 0; i < NUM_CORES; ++i)
        {
            io_contexts_.push_back(std::make_unique<boost::asio::io_context>(1));
            io_ptrs_.push_back(io_contexts_.back().get());
        }
        mesh_ = std::make_unique<SpscMesh>(NUM_CORES, QUEUE_CAPACITY, io_ptrs_);
    }

    std::vector<std::unique_ptr<boost::asio::io_context>> io_contexts_;
    std::vector<boost::asio::io_context*> io_ptrs_;
    std::unique_ptr<SpscMesh> mesh_;
    CrossCoreDispatcher dispatcher_;
};

TEST_F(SpscMeshTest, QueueAccess_SrcDst)
{
    auto& q01 = mesh_->queue(0, 1);
    auto& q10 = mesh_->queue(1, 0);

    EXPECT_NE(&q01, &q10);
    EXPECT_EQ(&mesh_->queue(0, 1), &q01);
}

TEST_F(SpscMeshTest, CoreCount)
{
    EXPECT_EQ(mesh_->core_count(), NUM_CORES);
}

TEST_F(SpscMeshTest, SingleCoreMode)
{
    boost::asio::io_context single_io{1};
    std::vector<boost::asio::io_context*> single_ptrs{&single_io};
    SpscMesh mesh(1, 64, single_ptrs);
    EXPECT_EQ(mesh.core_count(), 1u);

    auto count = mesh.drain_all_for(0, dispatcher_, nullptr, 1024);
    EXPECT_EQ(count, 0u);
}

TEST_F(SpscMeshTest, DrainAllFor_ReceivesFromMultipleSources)
{
    CoreMessage msg0{.op = CrossCoreOp::Noop, .source_core = 0, .data = 100};
    CoreMessage msg2{.op = CrossCoreOp::Noop, .source_core = 2, .data = 200};

    mesh_->queue(0, 1).try_enqueue(msg0);
    mesh_->queue(2, 1).try_enqueue(msg2);

    std::vector<std::pair<uint32_t, uintptr_t>> received;
    auto handler = [&](uint32_t /*core_id*/, const CoreMessage& msg) {
        received.emplace_back(msg.source_core, msg.data);
    };

    auto count = mesh_->drain_all_for(1, dispatcher_, handler, 1024);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(received.size(), 2u);
}

TEST_F(SpscMeshTest, DrainAllFor_BatchLimit)
{
    for (int i = 0; i < 10; ++i)
    {
        CoreMessage msg{.op = CrossCoreOp::Noop, .source_core = 0, .data = static_cast<uintptr_t>(i)};
        mesh_->queue(0, 1).try_enqueue(msg);
    }

    auto count = mesh_->drain_all_for(1, dispatcher_, nullptr, 5);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(mesh_->queue(0, 1).size_approx(), 5u);
}

TEST_F(SpscMeshTest, Shutdown_CleansLegacyClosures)
{
    // Verify no leak — ASAN catches if not deleted
    auto* task = new std::function<void()>([] {});

    CoreMessage msg{.op = CrossCoreOp::LegacyCrossCoreFn,
                    .source_core = 0,
                    .data = reinterpret_cast<uintptr_t>(task)};
    mesh_->queue(0, 1).try_enqueue(msg);

    mesh_->shutdown();
    // If task was not deleted during shutdown, ASAN will report a leak
}

TEST_F(SpscMeshTest, DrainAllFor_DispatchesRegisteredOp)
{
    std::atomic<int> received_value{0};
    dispatcher_.register_handler(static_cast<CrossCoreOp>(0x0100),
                                 +[](uint32_t /*core_id*/, uint32_t /*source_core*/, void* data) {
                                     auto* val = reinterpret_cast<std::atomic<int>*>(data);
                                     val->store(42, std::memory_order_relaxed);
                                 });

    CoreMessage msg{.op = static_cast<CrossCoreOp>(0x0100),
                    .source_core = 0,
                    .data = reinterpret_cast<uintptr_t>(&received_value)};
    mesh_->queue(0, 1).try_enqueue(msg);

    (void)mesh_->drain_all_for(1, dispatcher_, nullptr, 1024);
    EXPECT_EQ(received_value.load(), 42);
}

} // namespace apex::core
