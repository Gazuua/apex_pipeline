#include <apex/shared/rate_limit/endpoint_rate_config.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::rate_limit;

TEST(EndpointRateConfig, DefaultLimit) {
    EndpointRateConfig config{.default_limit = 60};
    EXPECT_EQ(config.limit_for(9999), 60u);
}

TEST(EndpointRateConfig, OverrideLimit) {
    EndpointRateConfig config{.default_limit = 60};
    config.overrides[1001] = 10;   // LoginRequest
    config.overrides[2001] = 200;  // ChatSendMessage

    EXPECT_EQ(config.limit_for(1001), 10u);
    EXPECT_EQ(config.limit_for(2001), 200u);
    EXPECT_EQ(config.limit_for(3000), 60u);  // No override -> default
}

TEST(EndpointRateConfig, EmptyOverrides) {
    EndpointRateConfig config{.default_limit = 100};
    // All msg_ids use default
    for (uint32_t id = 0; id < 100; ++id) {
        EXPECT_EQ(config.limit_for(id), 100u);
    }
}

TEST(EndpointRateConfig, ZeroDefault) {
    EndpointRateConfig config{.default_limit = 0};
    config.overrides[1001] = 10;

    EXPECT_EQ(config.limit_for(1001), 10u);
    EXPECT_EQ(config.limit_for(9999), 0u);  // Effectively disabled
}

TEST(EndpointRateConfig, WindowSize) {
    EndpointRateConfig config{.window_size = std::chrono::seconds{30}};
    EXPECT_EQ(config.window_size, std::chrono::seconds{30});
}
