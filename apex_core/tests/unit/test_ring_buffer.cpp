#include <apex/core/ring_buffer.hpp>
#include <cstring>
#include <gtest/gtest.h>

using namespace apex::core;

TEST(RingBuffer, Construction)
{
    RingBuffer rb(1024);
    EXPECT_EQ(rb.capacity(), 1024u);
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), 1024u);
}

TEST(RingBuffer, CapacityRoundsUp)
{
    RingBuffer rb(1000);
    EXPECT_EQ(rb.capacity(), 1024u);
}

TEST(RingBuffer, WriteAndRead)
{
    RingBuffer rb(64);
    const uint8_t data[] = {1, 2, 3, 4, 5};

    auto w = rb.writable();
    ASSERT_GE(w.size(), sizeof(data));
    std::memcpy(w.data(), data, sizeof(data));
    rb.commit_write(sizeof(data));

    EXPECT_EQ(rb.readable_size(), 5u);

    auto r = rb.contiguous_read();
    ASSERT_GE(r.size(), 5u);
    EXPECT_EQ(r[0], 1);
    EXPECT_EQ(r[4], 5);

    rb.consume(5);
    EXPECT_EQ(rb.readable_size(), 0u);
}

TEST(RingBuffer, WrapAround)
{
    RingBuffer rb(8);
    uint8_t data4[4] = {10, 20, 30, 40};

    // Write 4 bytes then consume to advance positions
    auto w = rb.writable();
    std::memcpy(w.data(), data4, 4);
    rb.commit_write(4);
    rb.consume(4);

    // Now write 6 bytes which will wrap around
    uint8_t data6[6] = {1, 2, 3, 4, 5, 6};
    w = rb.writable();
    size_t first = std::min(w.size(), size_t(6));
    std::memcpy(w.data(), data6, first);
    rb.commit_write(first);

    if (first < 6)
    {
        w = rb.writable();
        std::memcpy(w.data(), data6 + first, 6 - first);
        rb.commit_write(6 - first);
    }

    EXPECT_EQ(rb.readable_size(), 6u);
    auto r = rb.contiguous_read();
    EXPECT_LE(r.size(), 6u);
}

TEST(RingBuffer, Linearize_Contiguous)
{
    RingBuffer rb(64);
    uint8_t data[] = {1, 2, 3, 4, 5};
    auto w = rb.writable();
    std::memcpy(w.data(), data, 5);
    rb.commit_write(5);

    auto span = rb.linearize(5);
    ASSERT_EQ(span.size(), 5u);
    EXPECT_EQ(span[0], 1);
    EXPECT_EQ(span[4], 5);
}

TEST(RingBuffer, Linearize_WrapAround)
{
    RingBuffer rb(8);

    // Fill and consume 6 bytes to advance read/write positions
    uint8_t fill[6] = {0};
    auto w = rb.writable();
    std::memcpy(w.data(), fill, 6);
    rb.commit_write(6);
    rb.consume(6);

    // Write 4 bytes — this will wrap around (positions 6,7,0,1)
    uint8_t data[4] = {10, 20, 30, 40};
    w = rb.writable();
    size_t first = std::min(w.size(), size_t(4));
    std::memcpy(w.data(), data, first);
    rb.commit_write(first);
    if (first < 4)
    {
        w = rb.writable();
        std::memcpy(w.data(), data + first, 4 - first);
        rb.commit_write(4 - first);
    }

    auto span = rb.linearize(4);
    ASSERT_EQ(span.size(), 4u);
    EXPECT_EQ(span[0], 10);
    EXPECT_EQ(span[1], 20);
    EXPECT_EQ(span[2], 30);
    EXPECT_EQ(span[3], 40);
}

// T3: Wrap-around data integrity - verify all bytes after linearize
TEST(RingBuffer, WrapAroundDataIntegrity)
{
    RingBuffer rb(8);

    // Advance write/read positions to force wrap
    uint8_t filler[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto w = rb.writable();
    std::memcpy(w.data(), filler, 6);
    rb.commit_write(6);
    rb.consume(6);

    // Write 6 bytes that wrap around: positions 6,7,0,1,2,3
    uint8_t data[6] = {10, 20, 30, 40, 50, 60};
    w = rb.writable();
    size_t first = std::min(w.size(), size_t(6));
    std::memcpy(w.data(), data, first);
    rb.commit_write(first);
    if (first < 6)
    {
        w = rb.writable();
        std::memcpy(w.data(), data + first, 6 - first);
        rb.commit_write(6 - first);
    }

    EXPECT_EQ(rb.readable_size(), 6u);

    // Linearize and verify every byte
    auto span = rb.linearize(6);
    ASSERT_EQ(span.size(), 6u);
    EXPECT_EQ(span[0], 10);
    EXPECT_EQ(span[1], 20);
    EXPECT_EQ(span[2], 30);
    EXPECT_EQ(span[3], 40);
    EXPECT_EQ(span[4], 50);
    EXPECT_EQ(span[5], 60);
}

TEST(RingBuffer, Linearize_NotEnoughData)
{
    RingBuffer rb(16);
    auto span = rb.linearize(5);
    EXPECT_TRUE(span.empty());
}

TEST(RingBuffer, WritableEmptyWhenFull)
{
    RingBuffer rb(8);
    // Fill the entire buffer
    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto w = rb.writable();
    ASSERT_GE(w.size(), 8u);
    std::memcpy(w.data(), data, 8);
    rb.commit_write(8);

    EXPECT_TRUE(rb.writable().empty());
    EXPECT_EQ(rb.writable_size(), 0u);
    EXPECT_EQ(rb.readable_size(), 8u);
}

TEST(RingBuffer, MinCapacityOneWorks)
{
    RingBuffer rb(1);
    // capacity=1 rounds up to 1 (power of 2)
    EXPECT_GE(rb.capacity(), 1u);

    // Write 1 byte
    uint8_t byte = 0xAB;
    auto w = rb.writable();
    ASSERT_GE(w.size(), 1u);
    std::memcpy(w.data(), &byte, 1);
    rb.commit_write(1);

    EXPECT_EQ(rb.readable_size(), 1u);
    EXPECT_EQ(rb.writable_size(), 0u);

    // Read it back
    auto r = rb.contiguous_read();
    ASSERT_GE(r.size(), 1u);
    EXPECT_EQ(r[0], 0xAB);

    // Consume and verify wrap works
    rb.consume(1);
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), rb.capacity());

    // Write again after wrap
    byte = 0xCD;
    w = rb.writable();
    ASSERT_GE(w.size(), 1u);
    std::memcpy(w.data(), &byte, 1);
    rb.commit_write(1);

    r = rb.contiguous_read();
    ASSERT_GE(r.size(), 1u);
    EXPECT_EQ(r[0], 0xCD);
    rb.consume(1);
}

TEST(RingBuffer, Reset)
{
    RingBuffer rb(16);
    uint8_t d[8] = {};
    auto w = rb.writable();
    std::memcpy(w.data(), d, 8);
    rb.commit_write(8);

    rb.reset();
    EXPECT_EQ(rb.readable_size(), 0u);
    EXPECT_EQ(rb.writable_size(), 16u);
}

// --- capacity=0 boundary test ---

TEST(RingBuffer, CapacityZeroRoundsUp)
{
    // next_power_of_2(0) returns 1, but RingBuffer constructor clamps to max(1, capacity)
    // before calling next_power_of_2, so capacity=0 → 1.
    RingBuffer rb(0);
    EXPECT_GE(rb.capacity(), 1u);

    // Verify it is still functional
    uint8_t byte = 0x42;
    auto w = rb.writable();
    ASSERT_GE(w.size(), 1u);
    std::memcpy(w.data(), &byte, 1);
    rb.commit_write(1);

    EXPECT_EQ(rb.readable_size(), 1u);
    auto r = rb.contiguous_read();
    ASSERT_GE(r.size(), 1u);
    EXPECT_EQ(r[0], 0x42);
    rb.consume(1);
    EXPECT_EQ(rb.readable_size(), 0u);
}

// --- linearize → partial consume → linearize composite test ---

TEST(RingBuffer, LinearizeConsumeLinearizeComposite)
{
    RingBuffer rb(8);

    // Advance positions to force wrap-around
    uint8_t filler[6] = {};
    auto w = rb.writable();
    std::memcpy(w.data(), filler, 6);
    rb.commit_write(6);
    rb.consume(6);

    // Write 6 bytes that wrap: positions 6,7,0,1,2,3
    uint8_t data[6] = {10, 20, 30, 40, 50, 60};
    w = rb.writable();
    size_t first = std::min(w.size(), size_t(6));
    std::memcpy(w.data(), data, first);
    rb.commit_write(first);
    if (first < 6)
    {
        w = rb.writable();
        std::memcpy(w.data(), data + first, 6 - first);
        rb.commit_write(6 - first);
    }
    ASSERT_EQ(rb.readable_size(), 6u);

    // First linearize — full 6 bytes
    auto span1 = rb.linearize(6);
    ASSERT_EQ(span1.size(), 6u);
    EXPECT_EQ(span1[0], 10);
    EXPECT_EQ(span1[5], 60);

    // Partial consume — consume first 3 bytes
    rb.consume(3);
    EXPECT_EQ(rb.readable_size(), 3u);

    // Second linearize — remaining 3 bytes
    auto span2 = rb.linearize(3);
    ASSERT_EQ(span2.size(), 3u);
    EXPECT_EQ(span2[0], 40);
    EXPECT_EQ(span2[1], 50);
    EXPECT_EQ(span2[2], 60);
}

// --- write() method tests ---

TEST(RingBuffer, WriteMethodBasic)
{
    RingBuffer rb(16);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    EXPECT_TRUE(rb.write(data));
    EXPECT_EQ(rb.readable_size(), 5u);

    auto r = rb.contiguous_read();
    ASSERT_GE(r.size(), 5u);
    EXPECT_EQ(r[0], 1);
    EXPECT_EQ(r[4], 5);
}

TEST(RingBuffer, WriteMethodWrapAround)
{
    RingBuffer rb(8);

    // Advance to force wrap
    uint8_t filler[5] = {};
    auto w = rb.writable();
    std::memcpy(w.data(), filler, 5);
    rb.commit_write(5);
    rb.consume(5);

    // write() 6 bytes that will wrap around
    std::vector<uint8_t> data = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
    EXPECT_TRUE(rb.write(data));
    EXPECT_EQ(rb.readable_size(), 6u);

    // Verify via linearize
    auto span = rb.linearize(6);
    ASSERT_EQ(span.size(), 6u);
    for (size_t i = 0; i < 6; ++i)
    {
        EXPECT_EQ(span[i], data[i]);
    }
}

TEST(RingBuffer, WriteMethodInsufficientSpace)
{
    RingBuffer rb(8);

    // Fill 6 of 8 bytes
    uint8_t filler[6] = {};
    auto w = rb.writable();
    std::memcpy(w.data(), filler, 6);
    rb.commit_write(6);
    EXPECT_EQ(rb.writable_size(), 2u);

    // Try to write 4 bytes — should fail
    std::vector<uint8_t> data = {1, 2, 3, 4};
    EXPECT_FALSE(rb.write(data));

    // Buffer state should be unchanged
    EXPECT_EQ(rb.readable_size(), 6u);
    EXPECT_EQ(rb.writable_size(), 2u);
}
