// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/chat_svc/chat_service.hpp>
#include <gtest/gtest.h>

// Tests verify ChatService construction and configuration only.
// Full handler tests require mock adapters (covered in E2E, Plan 5).
// FlatBuffers schema tests are in chat_room_test.cpp / chat_message_test.cpp.

namespace
{

// NOTE: ChatService constructor now takes Config only (adapter refs from on_configure).
// These tests verify Config struct defaults and accessors.

TEST(ChatServiceConfigTest, DefaultConfig)
{
    apex::chat_svc::ChatService::Config cfg;
    EXPECT_EQ(cfg.request_topic, "chat.requests");
    EXPECT_EQ(cfg.response_topic, "chat.responses");
    EXPECT_EQ(cfg.persist_topic, "chat.messages.persist");
    EXPECT_EQ(cfg.max_room_members, 100u);
    EXPECT_EQ(cfg.max_message_length, 2000u);
    EXPECT_EQ(cfg.history_page_size, 50u);
}

TEST(ChatServiceConfigTest, CustomConfig)
{
    apex::chat_svc::ChatService::Config cfg{
        .request_topic = "test.chat.requests",
        .response_topic = "test.chat.responses",
        .persist_topic = "test.chat.messages.persist",
        .max_room_members = 50,
        .max_message_length = 1000,
        .history_page_size = 20,
    };
    EXPECT_EQ(cfg.request_topic, "test.chat.requests");
    EXPECT_EQ(cfg.response_topic, "test.chat.responses");
    EXPECT_EQ(cfg.persist_topic, "test.chat.messages.persist");
    EXPECT_EQ(cfg.max_room_members, 50u);
    EXPECT_EQ(cfg.max_message_length, 1000u);
    EXPECT_EQ(cfg.history_page_size, 20u);
}

TEST(ChatServiceTest, ConstructionAndName)
{
    apex::chat_svc::ChatService::Config cfg;
    apex::chat_svc::ChatService svc(std::move(cfg));
    EXPECT_EQ(svc.name(), "chat");
    EXPECT_FALSE(svc.started());
}

} // namespace
