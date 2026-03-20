// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/redis/redis_reply.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace apex::shared::adapters::redis;

// Helper to create a mock redisReply on the heap.
// The caller owns the memory and must clean up with free_mock_reply().
struct MockReplyBuilder
{
    // Owns all allocated replies for cleanup
    std::vector<redisReply*> owned;

    redisReply* make_string(const char* s)
    {
        auto* r = new redisReply{};
        r->type = REDIS_REPLY_STRING;
        r->len = static_cast<size_t>(std::strlen(s));
        r->str = new char[r->len + 1];
        std::memcpy(r->str, s, r->len + 1);
        owned.push_back(r);
        return r;
    }

    redisReply* make_status(const char* s)
    {
        auto* r = make_string(s);
        r->type = REDIS_REPLY_STATUS;
        return r;
    }

    redisReply* make_integer(int64_t val)
    {
        auto* r = new redisReply{};
        r->type = REDIS_REPLY_INTEGER;
        r->integer = val;
        r->str = nullptr;
        owned.push_back(r);
        return r;
    }

    redisReply* make_nil()
    {
        auto* r = new redisReply{};
        r->type = REDIS_REPLY_NIL;
        r->str = nullptr;
        owned.push_back(r);
        return r;
    }

    redisReply* make_error(const char* s)
    {
        auto* r = make_string(s);
        r->type = REDIS_REPLY_ERROR;
        return r;
    }

    redisReply* make_array(std::vector<redisReply*> elements)
    {
        auto* r = new redisReply{};
        r->type = REDIS_REPLY_ARRAY;
        r->elements = elements.size();
        r->str = nullptr;
        if (!elements.empty())
        {
            r->element = new redisReply*[elements.size()];
            for (size_t i = 0; i < elements.size(); ++i)
            {
                r->element[i] = elements[i];
            }
        }
        else
        {
            r->element = nullptr;
        }
        owned.push_back(r);
        return r;
    }

    ~MockReplyBuilder()
    {
        for (auto* r : owned)
        {
            if (r->type == REDIS_REPLY_ARRAY && r->element)
            {
                delete[] r->element; // element array only, children owned separately
            }
            if (r->str)
                delete[] r->str;
            delete r;
        }
    }
};

// TC1: STRING type parsing
TEST(RedisReplyTest, StringType)
{
    MockReplyBuilder builder;
    auto* raw = builder.make_string("hello");
    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_string());
    EXPECT_FALSE(reply.is_integer());
    EXPECT_FALSE(reply.is_array());
    EXPECT_FALSE(reply.is_nil());
    EXPECT_FALSE(reply.is_error());
    EXPECT_EQ(reply.str, "hello");
    EXPECT_EQ(reply.type, REDIS_REPLY_STRING);
}

// TC1b: STATUS type also counts as string
TEST(RedisReplyTest, StatusType)
{
    MockReplyBuilder builder;
    auto* raw = builder.make_status("OK");
    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_string());
    EXPECT_EQ(reply.str, "OK");
}

// TC2: INTEGER type
TEST(RedisReplyTest, IntegerType)
{
    MockReplyBuilder builder;
    auto* raw = builder.make_integer(42);
    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_integer());
    EXPECT_FALSE(reply.is_string());
    EXPECT_EQ(reply.integer, 42);
}

// TC3: ARRAY type — recursive parsing, array.size() verification
TEST(RedisReplyTest, ArrayType)
{
    MockReplyBuilder builder;
    auto* elem0 = builder.make_string("one");
    auto* elem1 = builder.make_integer(2);
    auto* elem2 = builder.make_nil();
    auto* raw = builder.make_array({elem0, elem1, elem2});

    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_array());
    ASSERT_EQ(reply.array.size(), 3u);
    EXPECT_TRUE(reply.array[0].is_string());
    EXPECT_EQ(reply.array[0].str, "one");
    EXPECT_TRUE(reply.array[1].is_integer());
    EXPECT_EQ(reply.array[1].integer, 2);
    EXPECT_TRUE(reply.array[2].is_nil());
}

// TC4: NIL type
TEST(RedisReplyTest, NilType)
{
    MockReplyBuilder builder;
    auto* raw = builder.make_nil();
    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_nil());
    EXPECT_FALSE(reply.is_string());
    EXPECT_FALSE(reply.is_array());
}

// TC5: Nested ARRAY parsing
TEST(RedisReplyTest, NestedArray)
{
    MockReplyBuilder builder;
    auto* inner_elem = builder.make_string("inner");
    auto* inner_array = builder.make_array({inner_elem});
    auto* outer_elem = builder.make_integer(99);
    auto* raw = builder.make_array({outer_elem, inner_array});

    RedisReply reply(raw);

    ASSERT_TRUE(reply.is_array());
    ASSERT_EQ(reply.array.size(), 2u);
    EXPECT_TRUE(reply.array[0].is_integer());
    EXPECT_EQ(reply.array[0].integer, 99);
    ASSERT_TRUE(reply.array[1].is_array());
    ASSERT_EQ(reply.array[1].array.size(), 1u);
    EXPECT_TRUE(reply.array[1].array[0].is_string());
    EXPECT_EQ(reply.array[1].array[0].str, "inner");
}

// TC6: nullptr reply
TEST(RedisReplyTest, NullptrReply)
{
    RedisReply reply(nullptr);
    EXPECT_EQ(reply.type, 0); // zero-initialized (no hiredis type constant for nullptr)
    EXPECT_TRUE(reply.str.empty());
    EXPECT_TRUE(reply.array.empty());
}

// TC7: ERROR type
TEST(RedisReplyTest, ErrorType)
{
    MockReplyBuilder builder;
    auto* raw = builder.make_error("ERR something went wrong");
    RedisReply reply(raw);

    EXPECT_TRUE(reply.is_error());
    EXPECT_FALSE(reply.is_string());
    EXPECT_EQ(reply.str, "ERR something went wrong");
}
