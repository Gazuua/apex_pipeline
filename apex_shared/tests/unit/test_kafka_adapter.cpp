// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/core_engine.hpp>
#include <apex/core/error_code.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/adapters/adapter_base.hpp>
#include <apex/shared/adapters/kafka/kafka_adapter.hpp>

#include <gtest/gtest.h>

using namespace apex::shared::adapters::kafka;
using apex::core::ErrorCode;

TEST(KafkaAdapter, NameIsKafka)
{
    KafkaConfig config;
    KafkaAdapter adapter(config);
    EXPECT_EQ(adapter.name(), "kafka");
}

TEST(KafkaAdapter, NotReadyBeforeInit)
{
    KafkaConfig config;
    KafkaAdapter adapter(config);
    EXPECT_FALSE(adapter.is_ready());
}

TEST(KafkaAdapter, InitMakesReady)
{
    KafkaConfig config;
    config.brokers = "localhost:9092";

    // engine must outlive adapter (adapter holds io_context references)
    apex::core::CoreEngineConfig engine_config{.num_cores = 2,
                                               .mpsc_queue_capacity = 64,
                                               .tick_interval = std::chrono::milliseconds{100},
                                               .drain_batch_limit = 1024};
    apex::core::CoreEngine engine(engine_config);
    KafkaAdapter adapter(config);

    adapter.init(engine); // AdapterBase::init -> do_init
    EXPECT_TRUE(adapter.is_ready());
}

TEST(KafkaAdapter, DrainMakesNotReady)
{
    KafkaConfig config;
    config.brokers = "localhost:9092";

    apex::core::CoreEngineConfig engine_config{.num_cores = 1,
                                               .mpsc_queue_capacity = 64,
                                               .tick_interval = std::chrono::milliseconds{100},
                                               .drain_batch_limit = 1024};
    apex::core::CoreEngine engine(engine_config);
    KafkaAdapter adapter(config);

    adapter.init(engine);
    EXPECT_TRUE(adapter.is_ready());

    adapter.drain();
    EXPECT_FALSE(adapter.is_ready());
}

TEST(KafkaAdapter, ProduceAfterDrainFails)
{
    KafkaConfig config;
    config.brokers = "localhost:9092";

    apex::core::CoreEngineConfig engine_config{.num_cores = 1,
                                               .mpsc_queue_capacity = 64,
                                               .tick_interval = std::chrono::milliseconds{100},
                                               .drain_batch_limit = 1024};
    apex::core::CoreEngine engine(engine_config);
    KafkaAdapter adapter(config);

    adapter.init(engine);
    adapter.drain();

    auto result = adapter.produce("topic", "key", "value");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::AdapterError);
}

TEST(KafkaAdapter, ProduceBeforeInitFails)
{
    KafkaConfig config;
    KafkaAdapter adapter(config);

    auto result = adapter.produce("topic", "key", "value");
    EXPECT_FALSE(result.has_value());
}

TEST(KafkaAdapter, AdapterWrapperTypeErasure)
{
    using apex::core::AdapterInterface;
    using apex::shared::adapters::AdapterWrapper;

    KafkaConfig config;
    auto wrapper = std::make_unique<AdapterWrapper<KafkaAdapter>>(config);
    AdapterInterface* iface = wrapper.get();

    EXPECT_EQ(iface->name(), "kafka");
    EXPECT_FALSE(iface->is_ready());
}

TEST(KafkaAdapter, ConfigAccessible)
{
    KafkaConfig config;
    config.brokers = "my-broker:9092";
    KafkaAdapter adapter(config);
    EXPECT_EQ(adapter.config().brokers, "my-broker:9092");
}

TEST(KafkaAdapter, SetMessageCallback)
{
    KafkaConfig config;
    KafkaAdapter adapter(config);

    bool cb_set = false;
    adapter.set_message_callback([&](std::string_view, int32_t, std::span<const uint8_t>, std::span<const uint8_t>,
                                     int64_t) -> apex::core::Result<void> {
        cb_set = true;
        return {};
    });

    // Callback is passed to Consumers during init -- only verify setup here
    EXPECT_FALSE(cb_set);
}
