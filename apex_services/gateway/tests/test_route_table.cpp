// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/gateway/route_table.hpp>

#include <gtest/gtest.h>

using namespace apex::gateway;

TEST(RouteTable, BasicResolve)
{
    auto table = RouteTable::build({
        {1000, 1999, "auth.requests"},
        {2000, 2999, "chat.requests"},
    });
    ASSERT_TRUE(table.has_value());

    EXPECT_EQ(table->resolve(1000).value(), "auth.requests");
    EXPECT_EQ(table->resolve(1500).value(), "auth.requests");
    EXPECT_EQ(table->resolve(1999).value(), "auth.requests");
    EXPECT_EQ(table->resolve(2000).value(), "chat.requests");
    EXPECT_EQ(table->resolve(2500).value(), "chat.requests");
}

TEST(RouteTable, UnmappedMsgId)
{
    auto table = RouteTable::build({
        {1000, 1999, "auth.requests"},
    });
    ASSERT_TRUE(table.has_value());

    EXPECT_FALSE(table->resolve(999).has_value());  // Before range
    EXPECT_FALSE(table->resolve(2000).has_value()); // After range
    EXPECT_FALSE(table->resolve(500).has_value());  // System range
}

TEST(RouteTable, OverlapRejected)
{
    auto table = RouteTable::build({
        {1000, 2000, "a"},
        {1500, 2500, "b"}, // Overlapping
    });
    EXPECT_FALSE(table.has_value());
}

TEST(RouteTable, InvalidRangeRejected)
{
    auto table = RouteTable::build({
        {2000, 1000, "a"}, // begin > end
    });
    EXPECT_FALSE(table.has_value());
}

TEST(RouteTable, NonContiguousRanges)
{
    auto table = RouteTable::build({
        {1000, 1999, "auth.requests"},
        {3000, 3999, "game.requests"}, // Gap at 2000-2999 (allowed)
    });
    ASSERT_TRUE(table.has_value());

    EXPECT_EQ(table->resolve(1500).value(), "auth.requests");
    EXPECT_FALSE(table->resolve(2500).has_value()); // In gap
    EXPECT_EQ(table->resolve(3500).value(), "game.requests");
}

TEST(RouteTable, SingleEntry)
{
    auto table = RouteTable::build({
        {5000, 5000, "single"}, // Single msg_id
    });
    ASSERT_TRUE(table.has_value());

    EXPECT_EQ(table->resolve(5000).value(), "single");
    EXPECT_FALSE(table->resolve(4999).has_value());
    EXPECT_FALSE(table->resolve(5001).has_value());
}

TEST(RouteTable, EmptyTable)
{
    auto table = RouteTable::build({});
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->size(), 0u);
    EXPECT_FALSE(table->resolve(1000).has_value());
}

TEST(RouteTable, UnsortedInput)
{
    // Input in reverse order -- should be sorted internally
    auto table = RouteTable::build({
        {3000, 3999, "chat.requests"},
        {1000, 1999, "auth.requests"},
    });
    ASSERT_TRUE(table.has_value());

    EXPECT_EQ(table->resolve(1500).value(), "auth.requests");
    EXPECT_EQ(table->resolve(3500).value(), "chat.requests");
}
