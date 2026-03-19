#include <apex/gateway/channel_session_map.hpp>

#include <apex/core/error_code.hpp>

#include <gtest/gtest.h>

using namespace apex::gateway;

TEST(ChannelSessionMap, SubscribeAndGet)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("pub:chat:room:1", 100).has_value());
    ASSERT_TRUE(map.subscribe("pub:chat:room:1", 200).has_value());

    auto* subs = map.get_subscribers("pub:chat:room:1");
    ASSERT_NE(subs, nullptr);
    EXPECT_EQ(subs->size(), 2u);
}

TEST(ChannelSessionMap, PerCoreLocalSubscribers)
{
    // Per-core 맵이므로 core별 그룹핑 불필요 — 모든 세션이 같은 맵에 로컬.
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 200).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 300).has_value());

    auto* subs = map.get_subscribers("ch1");
    ASSERT_NE(subs, nullptr);
    EXPECT_EQ(subs->size(), 3u);
}

TEST(ChannelSessionMap, Unsubscribe)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 200).has_value());
    map.unsubscribe("ch1", 100);

    auto* subs = map.get_subscribers("ch1");
    ASSERT_NE(subs, nullptr);
    EXPECT_EQ(subs->size(), 1u);
}

TEST(ChannelSessionMap, UnsubscribeAll)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100).has_value());
    map.unsubscribe_all(100);

    EXPECT_EQ(map.get_subscribers("ch1"), nullptr);
    EXPECT_EQ(map.get_subscribers("ch2"), nullptr);
}

TEST(ChannelSessionMap, EmptyChannel)
{
    ChannelSessionMap map;
    EXPECT_EQ(map.get_subscribers("nonexistent"), nullptr);
}

TEST(ChannelSessionMap, DuplicateSubscribe)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value()); // Duplicate — still ok

    auto* subs = map.get_subscribers("ch1");
    ASSERT_NE(subs, nullptr);
    EXPECT_EQ(subs->size(), 1u); // No duplicate
}

TEST(ChannelSessionMap, UnsubscribeFromEmptyChannel)
{
    ChannelSessionMap map;
    // Should not crash
    map.unsubscribe("nonexistent", 100);
    map.unsubscribe_all(999);
}

TEST(ChannelSessionMap, SubscriptionLimitExceeded)
{
    constexpr uint32_t kLimit = 3;
    ChannelSessionMap map(kLimit);

    // Fill up to the limit with distinct channels
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch3", 100).has_value());

    // 4th channel should fail
    auto result = map.subscribe("ch4", 100);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), apex::core::ErrorCode::SubscriptionLimitExceeded);

    // Re-subscribing to an already-subscribed channel should still succeed
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
}

TEST(ChannelSessionMap, SubscriptionLimitZeroMeansUnlimited)
{
    ChannelSessionMap map(0); // unlimited
    for (int i = 0; i < 100; ++i)
    {
        ASSERT_TRUE(map.subscribe("ch" + std::to_string(i), 100).has_value());
    }
}

TEST(ChannelSessionMap, SubscriptionLimitUnsubscribeFreesSlot)
{
    constexpr uint32_t kLimit = 2;
    ChannelSessionMap map(kLimit);

    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100).has_value());

    // At limit — new channel fails
    ASSERT_FALSE(map.subscribe("ch3", 100).has_value());

    // Unsubscribe one — should free a slot
    map.unsubscribe("ch1", 100);
    ASSERT_TRUE(map.subscribe("ch3", 100).has_value());
}

TEST(ChannelSessionMap, SubscribedChannels)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("alpha", 100).has_value());
    ASSERT_TRUE(map.subscribe("beta", 200).has_value());
    ASSERT_TRUE(map.subscribe("alpha", 300).has_value());

    auto channels = map.subscribed_channels();
    EXPECT_EQ(channels.size(), 2u); // alpha, beta
}

TEST(ChannelSessionMap, TotalSubscriptions)
{
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 200).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100).has_value());

    EXPECT_EQ(map.total_subscriptions(), 3u);

    map.unsubscribe("ch1", 100);
    EXPECT_EQ(map.total_subscriptions(), 2u);
}
