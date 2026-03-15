#include <apex/gateway/channel_session_map.hpp>

#include <gtest/gtest.h>

using namespace apex::gateway;

TEST(ChannelSessionMap, SubscribeAndGet) {
    ChannelSessionMap map;
    map.subscribe("pub:chat:room:1", 100, 0);
    map.subscribe("pub:chat:room:1", 200, 1);

    auto groups = map.get_subscribers("pub:chat:room:1");
    ASSERT_EQ(groups.size(), 2u);

    size_t total = 0;
    for (auto& g : groups) total += g.session_ids.size();
    EXPECT_EQ(total, 2u);
}

TEST(ChannelSessionMap, SameCoreGrouping) {
    ChannelSessionMap map;
    map.subscribe("ch1", 100, 0);
    map.subscribe("ch1", 200, 0);
    map.subscribe("ch1", 300, 1);

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
    map.subscribe("ch1", 100, 0);
    map.subscribe("ch1", 200, 0);
    map.unsubscribe("ch1", 100);

    auto groups = map.get_subscribers("ch1");
    size_t total = 0;
    for (auto& g : groups) total += g.session_ids.size();
    EXPECT_EQ(total, 1u);
}

TEST(ChannelSessionMap, UnsubscribeAll) {
    ChannelSessionMap map;
    map.subscribe("ch1", 100, 0);
    map.subscribe("ch2", 100, 0);
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
    map.subscribe("ch1", 100, 0);
    map.subscribe("ch1", 100, 0);  // Duplicate

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
