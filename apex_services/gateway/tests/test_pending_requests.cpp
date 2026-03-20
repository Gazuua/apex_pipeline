// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/pending_requests.hpp>

#include <apex/core/session.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace apex::gateway;
using apex::core::make_session_id;

TEST(PendingRequestsMap, InsertAndExtract)
{
    PendingRequestsMap map(100);
    ASSERT_TRUE(map.insert(1, make_session_id(1000), 42).has_value());
    EXPECT_EQ(map.size(), 1u);

    auto entry = map.extract(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->session_id, make_session_id(1000));
    EXPECT_EQ(entry->original_msg_id, 42u);

    EXPECT_EQ(map.size(), 0u);
}

TEST(PendingRequestsMap, ExtractNonExistent)
{
    PendingRequestsMap map(100);
    auto entry = map.extract(999);
    EXPECT_FALSE(entry.has_value());
}

TEST(PendingRequestsMap, CapacityLimit)
{
    PendingRequestsMap map(2);
    ASSERT_TRUE(map.insert(1, make_session_id(100), 1).has_value());
    ASSERT_TRUE(map.insert(2, make_session_id(200), 2).has_value());
    EXPECT_FALSE(map.insert(3, make_session_id(300), 3).has_value()); // Full
}

TEST(PendingRequestsMap, SweepExpired)
{
    // Inject fake time source — no sleep needed
    auto fake_now = std::chrono::steady_clock::now();
    auto now_fn = [&]() { return fake_now; };

    PendingRequestsMap map(100, std::chrono::milliseconds{50}, now_fn);
    ASSERT_TRUE(map.insert(1, make_session_id(100), 1).has_value());
    ASSERT_TRUE(map.insert(2, make_session_id(200), 2).has_value());

    // Advance fake clock past timeout
    fake_now += std::chrono::milliseconds{100};

    size_t expired_count = 0;
    map.sweep_expired([&](uint64_t, const auto&) { ++expired_count; });

    EXPECT_EQ(expired_count, 2u);
    EXPECT_EQ(map.size(), 0u);
}

TEST(PendingRequestsMap, SweepKeepsUnexpired)
{
    auto fake_now = std::chrono::steady_clock::now();
    auto now_fn = [&]() { return fake_now; };

    PendingRequestsMap map(100, std::chrono::milliseconds{5000}, now_fn);
    ASSERT_TRUE(map.insert(1, make_session_id(100), 1).has_value());

    // Don't advance clock — entries should remain unexpired
    size_t expired_count = 0;
    map.sweep_expired([&](uint64_t, const auto&) { ++expired_count; });

    EXPECT_EQ(expired_count, 0u);
    EXPECT_EQ(map.size(), 1u);
}

TEST(PendingRequestsMap, ExtractIsOneShot)
{
    PendingRequestsMap map(100);
    ASSERT_TRUE(map.insert(1, make_session_id(100), 42).has_value());

    auto first = map.extract(1);
    ASSERT_TRUE(first.has_value());

    auto second = map.extract(1);
    EXPECT_FALSE(second.has_value()); // Already extracted
}
