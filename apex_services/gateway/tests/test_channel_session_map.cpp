#include <apex/gateway/channel_session_map.hpp>

#include <apex/core/error_code.hpp>

#include <gtest/gtest.h>

using namespace apex::gateway;

TEST(ChannelSessionMap, SubscribeAndGet) {
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("pub:chat:room:1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("pub:chat:room:1", 200, 1).has_value());

    auto groups = map.get_subscribers("pub:chat:room:1");
    ASSERT_EQ(groups.size(), 2u);

    size_t total = 0;
    for (auto& g : groups) total += g.session_ids.size();
    EXPECT_EQ(total, 2u);
}

TEST(ChannelSessionMap, SameCoreGrouping) {
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 200, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 300, 1).has_value());

    auto groups = map.get_subscribers("ch1");
    ASSERT_EQ(groups.size(), 2u);  // 2 cores

    size_t total = 0;
    for (auto& g : groups) {
        total += g.session_ids.size();
        if (g.core_id == 0) {
            EXPECT_EQ(g.session_ids.size(), 2u);
        } else {
            EXPECT_EQ(g.session_ids.size(), 1u);
        }
    }
    EXPECT_EQ(total, 3u);
}

TEST(ChannelSessionMap, Unsubscribe) {
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 200, 0).has_value());
    map.unsubscribe("ch1", 100);

    auto groups = map.get_subscribers("ch1");
    size_t total = 0;
    for (auto& g : groups) total += g.session_ids.size();
    EXPECT_EQ(total, 1u);
}

TEST(ChannelSessionMap, UnsubscribeAll) {
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100, 0).has_value());
    map.unsubscribe_all(100);

    EXPECT_TRUE(map.get_subscribers("ch1").empty());
    EXPECT_TRUE(map.get_subscribers("ch2").empty());
}

TEST(ChannelSessionMap, EmptyChannel) {
    ChannelSessionMap map;
    auto groups = map.get_subscribers("nonexistent");
    EXPECT_TRUE(groups.empty());
}

TEST(ChannelSessionMap, DuplicateSubscribe) {
    ChannelSessionMap map;
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());  // Duplicate — still ok

    auto groups = map.get_subscribers("ch1");
    size_t total = 0;
    for (auto& g : groups) total += g.session_ids.size();
    EXPECT_EQ(total, 1u);  // No duplicate
}

TEST(ChannelSessionMap, UnsubscribeFromEmptyChannel) {
    ChannelSessionMap map;
    // Should not crash
    map.unsubscribe("nonexistent", 100);
    map.unsubscribe_all(999);
}

TEST(ChannelSessionMap, SubscriptionLimitExceeded) {
    constexpr uint32_t kLimit = 3;
    ChannelSessionMap map(kLimit);

    // Fill up to the limit with distinct channels
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch3", 100, 0).has_value());

    // 4th channel should fail
    auto result = map.subscribe("ch4", 100, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(),
              apex::core::ErrorCode::SubscriptionLimitExceeded);

    // Re-subscribing to an already-subscribed channel should still succeed
    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
}

TEST(ChannelSessionMap, SubscriptionLimitZeroMeansUnlimited) {
    ChannelSessionMap map(0);  // unlimited
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(
            map.subscribe("ch" + std::to_string(i), 100, 0).has_value());
    }
}

TEST(ChannelSessionMap, SubscriptionLimitUnsubscribeFreesSlot) {
    constexpr uint32_t kLimit = 2;
    ChannelSessionMap map(kLimit);

    ASSERT_TRUE(map.subscribe("ch1", 100, 0).has_value());
    ASSERT_TRUE(map.subscribe("ch2", 100, 0).has_value());

    // At limit — new channel fails
    ASSERT_FALSE(map.subscribe("ch3", 100, 0).has_value());

    // Unsubscribe one — should free a slot
    map.unsubscribe("ch1", 100);
    ASSERT_TRUE(map.subscribe("ch3", 100, 0).has_value());
}
