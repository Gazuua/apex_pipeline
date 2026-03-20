// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/bump_allocator.hpp>
#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace apex::shared::adapters::pg;

// =============================================================================
// PgResult tests (no PostgreSQL server needed)
// =============================================================================

TEST(PgResult, DefaultIsInvalid)
{
    PgResult result;
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.row_count(), 0);
    EXPECT_EQ(result.column_count(), 0);
    EXPECT_EQ(result.affected_rows(), 0);
}

TEST(PgResult, NullResultErrorMessage)
{
    PgResult result;
    // Default result returns "no result" error message
    EXPECT_FALSE(result.error_message().empty());
}

TEST(PgResult, NullResultStatus)
{
    PgResult result;
    EXPECT_EQ(result.status(), PGRES_FATAL_ERROR);
}

TEST(PgResult, NullResultGet)
{
    PgResult result;
    EXPECT_EQ(result.get(), nullptr);
}

TEST(PgResult, NullResultValueAndNull)
{
    PgResult result;
    // Accessing value on null result should be safe
    EXPECT_TRUE(result.value(0, 0).empty());
    EXPECT_TRUE(result.is_null(0, 0));
}

TEST(PgResult, NullResultColumnName)
{
    PgResult result;
    EXPECT_TRUE(result.column_name(0).empty());
}

TEST(PgResult, MoveConstruction)
{
    PgResult r1;
    EXPECT_FALSE(static_cast<bool>(r1));

    PgResult r2(std::move(r1));
    EXPECT_FALSE(static_cast<bool>(r2));
}

TEST(PgResult, MoveAssignment)
{
    PgResult r1;
    PgResult r2;

    r2 = std::move(r1);
    EXPECT_FALSE(static_cast<bool>(r2));
}

// =============================================================================
// PgConnection tests (no PostgreSQL server needed)
// =============================================================================

TEST(PgConnection, NotConnectedByDefault)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_connected());
    EXPECT_FALSE(conn.is_valid());
}

TEST(PgConnection, CloseOnUnconnected)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    // close() on unconnected should not crash
    conn.close();
    EXPECT_FALSE(conn.is_connected());
}

TEST(PgConnection, DoubleClose)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    conn.close();
    conn.close(); // Should not crash
    EXPECT_FALSE(conn.is_connected());
}

TEST(PgConnection, MoveConstruction)
{
    boost::asio::io_context io_ctx;
    PgConnection conn1(io_ctx);
    PgConnection conn2(std::move(conn1));
    EXPECT_FALSE(conn2.is_connected());
    // conn1 is in moved-from state -- don't use it
}

TEST(PgConnection, MoveAssignment)
{
    boost::asio::io_context io_ctx;
    PgConnection conn1(io_ctx);
    PgConnection conn2(io_ctx);
    conn2 = std::move(conn1);
    EXPECT_FALSE(conn2.is_connected());
}

TEST(PgConnection, DestructorHandlesCleanup)
{
    boost::asio::io_context io_ctx;
    {
        PgConnection conn(io_ctx);
        // Destructor should call close() safely
    }
    // No crash = pass
}

// =============================================================================
// Poisoned state tests
// =============================================================================

TEST(PgConnection, MarkPoisoned)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_poisoned());
    conn.mark_poisoned();
    EXPECT_TRUE(conn.is_poisoned());
}

TEST(PgConnection, PoisonedStateNotSetByDefault)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx);
    EXPECT_FALSE(conn.is_poisoned());
}

// =============================================================================
// BumpAllocator injection tests
// =============================================================================

TEST(PgConnection, ConstructWithBumpAllocator)
{
    boost::asio::io_context io_ctx;
    apex::core::BumpAllocator alloc(4096);
    PgConnection conn(io_ctx, &alloc);
    // Construction should succeed without crash
    EXPECT_FALSE(conn.is_connected());
}

TEST(PgConnection, ConstructWithNullAllocator)
{
    boost::asio::io_context io_ctx;
    PgConnection conn(io_ctx, nullptr);
    // Equivalent to default constructor
    EXPECT_FALSE(conn.is_connected());
}

TEST(PgConnection, MovePreservesPoisonedState)
{
    boost::asio::io_context io_ctx;
    PgConnection conn1(io_ctx);
    conn1.mark_poisoned();
    EXPECT_TRUE(conn1.is_poisoned());

    PgConnection conn2(std::move(conn1));
    EXPECT_TRUE(conn2.is_poisoned());
}

TEST(PgConnection, MoveAssignmentPreservesPoisonedState)
{
    boost::asio::io_context io_ctx;
    PgConnection conn1(io_ctx);
    conn1.mark_poisoned();

    PgConnection conn2(io_ctx);
    EXPECT_FALSE(conn2.is_poisoned());

    conn2 = std::move(conn1);
    EXPECT_TRUE(conn2.is_poisoned());
}
