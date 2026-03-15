#include <apex/gateway/pending_requests.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace apex::gateway;

TEST(PendingRequestsMap, InsertAndExtract) {
    PendingRequestsMap map(100);
    ASSERT_TRUE(map.insert(1, 1000, 42).has_value());
    EXPECT_EQ(map.size(), 1u);

    auto entry = map.extract(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->session_id, 1000u);
    EXPECT_EQ(entry->original_msg_id, 42u);

    EXPECT_EQ(map.size(), 0u);
}

TEST(PendingRequestsMap, ExtractNonExistent) {
    PendingRequestsMap map(100);
    auto entry = map.extract(999);
    EXPECT_FALSE(entry.has_value());
}

TEST(PendingRequestsMap, CapacityLimit) {
    PendingRequestsMap map(2);
    ASSERT_TRUE(map.insert(1, 100, 1).has_value());
    ASSERT_TRUE(map.insert(2, 200, 2).has_value());
    EXPECT_FALSE(map.insert(3, 300, 3).has_value());  // Full
}

TEST(PendingRequestsMap, SweepExpired) {
    PendingRequestsMap map(100, std::chrono::milliseconds{50});
    ASSERT_TRUE(map.insert(1, 100, 1).has_value());
    ASSERT_TRUE(map.insert(2, 200, 2).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    size_t expired_count = 0;
    map.sweep_expired([&](uint64_t, const auto&) { ++expired_count; });

    EXPECT_EQ(expired_count, 2u);
    EXPECT_EQ(map.size(), 0u);
}

TEST(PendingRequestsMap, SweepKeepsUnexpired) {
    PendingRequestsMap map(100, std::chrono::milliseconds{5000});
    ASSERT_TRUE(map.insert(1, 100, 1).has_value());

    size_t expired_count = 0;
    map.sweep_expired([&](uint64_t, const auto&) { ++expired_count; });

    EXPECT_EQ(expired_count, 0u);
    EXPECT_EQ(map.size(), 1u);
}

TEST(PendingRequestsMap, ExtractIsOneShot) {
    PendingRequestsMap map(100);
    ASSERT_TRUE(map.insert(1, 100, 42).has_value());

    auto first = map.extract(1);
    ASSERT_TRUE(first.has_value());

    auto second = map.extract(1);
    EXPECT_FALSE(second.has_value());  // Already extracted
}
