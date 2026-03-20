// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/detail/math_utils.hpp>
#include <gtest/gtest.h>

namespace apex::core::detail
{

TEST(MathUtils, NextPowerOfTwo_Zero)
{
    EXPECT_EQ(next_power_of_2(0), 1u);
}

TEST(MathUtils, NextPowerOfTwo_One)
{
    EXPECT_EQ(next_power_of_2(1), 1u);
}

TEST(MathUtils, NextPowerOfTwo_AlreadyPow2)
{
    EXPECT_EQ(next_power_of_2(16), 16u);
    EXPECT_EQ(next_power_of_2(1024), 1024u);
}

TEST(MathUtils, NextPowerOfTwo_NonPow2)
{
    EXPECT_EQ(next_power_of_2(3), 4u);
    EXPECT_EQ(next_power_of_2(17), 32u);
    EXPECT_EQ(next_power_of_2(1000), 1024u);
}

TEST(MathUtils, NextPowerOfTwo_OverflowReturnsZero)
{
    EXPECT_EQ(next_power_of_2((SIZE_MAX >> 1) + 2), 0u);
}

} // namespace apex::core::detail
