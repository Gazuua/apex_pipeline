// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/timing_wheel.hpp>
#include <gtest/gtest.h>
#include <set>
#include <stdexcept>

using namespace apex::core;

TEST(TimingWheel, Construction)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });
    EXPECT_EQ(tw.active_count(), 0u);
    EXPECT_EQ(tw.current_tick(), 0u);
}

TEST(TimingWheel, ScheduleAndExpire)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(3);
    EXPECT_EQ(tw.active_count(), 1u);

    tw.tick(); // tick 0->1
    tw.tick(); // tick 1->2
    tw.tick(); // tick 2->3
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3->4 — should fire (deadline=3)
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, ScheduleAtTickZero)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    (void)tw.schedule(0);
    tw.tick();
    EXPECT_EQ(expired.size(), 1u);
}

TEST(TimingWheel, Cancel)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    tw.tick();
    tw.tick();
    EXPECT_TRUE(expired.empty());
}

TEST(TimingWheel, Reschedule)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.tick(); // tick 0->1

    tw.reschedule(id, 3); // now expires at tick 1 + 3 = tick 4

    tw.tick(); // tick 1->2
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 2->3
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3->4
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 4->5 — should fire (deadline=4)
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

TEST(TimingWheel, MultipleEntries_SameSlot)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(5);
    auto id2 = tw.schedule(5);
    auto id3 = tw.schedule(5);

    for (int i = 0; i < 6; ++i)
        tw.tick(); // deadline=5, fires on tick 5

    EXPECT_EQ(expired.size(), 3u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_TRUE(expired.contains(id2));
    EXPECT_TRUE(expired.contains(id3));
}

TEST(TimingWheel, WrapAround)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(8, [&](TimingWheel::EntryId id) { expired.insert(id); });

    for (int i = 0; i < 8; ++i)
        tw.tick();

    auto id = tw.schedule(3);
    tw.tick();
    tw.tick();
    tw.tick();
    tw.tick(); // deadline=11, fires on tick 11
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

// T4: Double cancel safety - second cancel should be no-op
TEST(TimingWheel, DoubleCancelSafe)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(5);
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    // Second cancel — must not crash or corrupt state
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    // Tick past deadline — must not fire
    for (int i = 0; i < 8; ++i)
        tw.tick();
    EXPECT_TRUE(expired.empty());
}

// T4b: Reschedule after expiry — should be safe (no-op or ignored)
TEST(TimingWheel, RescheduleAfterExpiry)
{
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(1);
    tw.tick(); // tick 0->1
    tw.tick(); // tick 1->2 — fires at tick 1
    ASSERT_EQ(expired.size(), 1u);

    // Reschedule an already-expired entry — must not crash
    tw.reschedule(id, 3);

    // Tick some more — should not double-fire
    for (int i = 0; i < 5; ++i)
        tw.tick();
    EXPECT_EQ(expired.size(), 1u); // still just the original fire
}

TEST(TimingWheel, MaxValidTimeout)
{
    // num_slots=64 (power of 2), max valid ticks_from_now = num_slots - 1 = 63
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(63); // max valid value (num_slots - 1)
    EXPECT_EQ(tw.active_count(), 1u);

    // Tick 63 times: should NOT fire yet (deadline = 63, fires when tick reaches 63)
    for (int i = 0; i < 63; ++i)
        tw.tick();
    EXPECT_TRUE(expired.empty());

    // One more tick should fire (tick passes the deadline)
    tw.tick();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, CallbackCanScheduleNewEntry)
{
    size_t expire_count = 0;
    TimingWheel::EntryId second_id = 0;

    TimingWheel tw(64, [&](TimingWheel::EntryId) {
        ++expire_count;
        if (expire_count == 1)
        {
            // Schedule a new entry from within the callback.
            // At this point current_tick_ is still 1 (pre-increment in tick()).
            // compute_deadline(2) = current_tick_ + 2 = 1 + 2 = 3
            second_id = tw.schedule(2);
        }
    });

    (void)tw.schedule(1); // deadline = 0 + 1 = 1
    EXPECT_EQ(tw.active_count(), 1u);

    // tick(): current_tick_=0, slot_idx=0, no match (deadline=1). current_tick_ -> 1
    tw.tick();
    EXPECT_EQ(expire_count, 0u);

    // tick(): current_tick_=1, fires first entry (deadline=1).
    // Callback schedules second entry with deadline=3. current_tick_ -> 2
    tw.tick();
    EXPECT_EQ(expire_count, 1u);
    EXPECT_NE(second_id, 0u);
    EXPECT_EQ(tw.active_count(), 1u);

    // tick(): current_tick_=2, no match. current_tick_ -> 3
    tw.tick();
    EXPECT_EQ(expire_count, 1u);

    // tick(): current_tick_=3, fires second entry (deadline=3). current_tick_ -> 4
    tw.tick();
    EXPECT_EQ(expire_count, 2u);
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, ScheduleOutOfRangeThrows)
{
    TimingWheel tw(64, [](TimingWheel::EntryId) {});

    // num_slots=64, max valid = 63
    EXPECT_THROW((void)tw.schedule(64), std::out_of_range);
    EXPECT_THROW((void)tw.schedule(100), std::out_of_range);
    EXPECT_THROW((void)tw.schedule(UINT32_MAX), std::out_of_range);

    // 경계값 — 63은 OK, 64부터 throw
    EXPECT_NO_THROW((void)tw.schedule(63));
    EXPECT_EQ(tw.active_count(), 1u);
}

TEST(TimingWheel, RescheduleOutOfRangeThrows)
{
    TimingWheel tw(64, [](TimingWheel::EntryId) {});

    auto id = tw.schedule(10);
    EXPECT_THROW(tw.reschedule(id, 64), std::out_of_range);
    EXPECT_THROW(tw.reschedule(id, 100), std::out_of_range);

    // 유효한 reschedule은 정상 동작
    EXPECT_NO_THROW(tw.reschedule(id, 63));
    EXPECT_EQ(tw.active_count(), 1u);
}

TEST(TimingWheel, LargeNumberOfEntries)
{
    size_t expire_count = 0;
    TimingWheel tw(256, [&](TimingWheel::EntryId) { ++expire_count; });

    for (int i = 1; i <= 1000; ++i)
    {
        (void)tw.schedule(i % 200 + 1);
    }
    EXPECT_EQ(tw.active_count(), 1000u);

    for (int i = 0; i < 250; ++i)
        tw.tick();
    EXPECT_EQ(expire_count, 1000u);
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, RescheduleWithOtherEntriesInOldSlot)
{
    // Verify linked list integrity when reschedule removes an entry from a slot
    // that contains other entries.
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    // Schedule 3 entries in the same slot (same deadline)
    auto id1 = tw.schedule(5);
    auto id2 = tw.schedule(5);
    auto id3 = tw.schedule(5);
    EXPECT_EQ(tw.active_count(), 3u);

    // Reschedule the middle entry (id2) to a different slot
    tw.reschedule(id2, 10);
    EXPECT_EQ(tw.active_count(), 3u); // still 3 active entries

    // Tick past the original deadline — id1 and id3 should expire
    for (int i = 0; i < 6; ++i)
        tw.tick();
    EXPECT_EQ(expired.size(), 2u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_TRUE(expired.contains(id3));
    EXPECT_FALSE(expired.contains(id2));

    // id2 should still be active
    EXPECT_EQ(tw.active_count(), 1u);

    // Tick until id2 expires (rescheduled to tick 10, fires on tick 10+1=11)
    for (int i = 0; i < 6; ++i)
        tw.tick(); // ticks 6..11
    EXPECT_TRUE(expired.contains(id2));
    EXPECT_EQ(expired.size(), 3u);
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, RescheduleFirstEntryInSlot)
{
    // Reschedule the head of a multi-entry slot
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(3);
    auto id2 = tw.schedule(3);
    EXPECT_EQ(tw.active_count(), 2u);

    // Reschedule id1 (first scheduled, could be head or tail depending on impl)
    tw.reschedule(id1, 8);
    EXPECT_EQ(tw.active_count(), 2u);

    // Tick past deadline 3 — only id2 should fire
    for (int i = 0; i < 4; ++i)
        tw.tick();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id2));

    // Tick until id1 fires
    for (int i = 0; i < 6; ++i)
        tw.tick();
    EXPECT_EQ(expired.size(), 2u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, CancelInsideCallback)
{
    // Verify that cancel() inside a tick callback does not corrupt state
    TimingWheel::EntryId id2 = 0;
    size_t expire_count = 0;

    TimingWheel tw(64, [&](TimingWheel::EntryId) {
        ++expire_count;
        if (expire_count == 1 && id2 != 0)
        {
            tw.cancel(id2);
        }
    });

    (void)tw.schedule(1); // fires at tick 1
    id2 = tw.schedule(2); // fires at tick 2 — will be cancelled in callback
    EXPECT_EQ(tw.active_count(), 2u);

    tw.tick(); // tick 0->1: no fire
    tw.tick(); // tick 1->2: first entry fires, cancels id2 inside callback
    EXPECT_EQ(expire_count, 1u);
    EXPECT_EQ(tw.active_count(), 0u);

    // tick past id2's original deadline — should NOT fire (was cancelled)
    tw.tick();
    tw.tick();
    EXPECT_EQ(expire_count, 1u);
}

TEST(TimingWheel, ScheduleInsideCallbackFiresOnCorrectTick)
{
    size_t fire_count = 0;
    TimingWheel tw(8, [&](TimingWheel::EntryId) {
        ++fire_count;
        if (fire_count == 1)
        {
            // schedule(2) from within callback.
            // At this point current_tick_ is still 1 (pre-increment in tick()).
            // So deadline = 1 + 2 = 3.
            (void)tw.schedule(2);
        }
    });

    (void)tw.schedule(1); // deadline = 0 + 1 = 1
    tw.tick();            // current_tick_=0, no match. current_tick_ -> 1
    EXPECT_EQ(fire_count, 0u);

    tw.tick(); // current_tick_=1, fires first entry. Callback schedules deadline=3. current_tick_ -> 2
    EXPECT_EQ(fire_count, 1u);
    EXPECT_EQ(tw.active_count(), 1u);

    tw.tick(); // current_tick_=2, no match. current_tick_ -> 3
    EXPECT_EQ(fire_count, 1u);

    tw.tick(); // current_tick_=3, fires second entry (deadline=3). current_tick_ -> 4
    EXPECT_EQ(fire_count, 2u);
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, RescheduleLastEntryInSlot)
{
    // Reschedule the tail of a multi-entry slot
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(3);
    auto id2 = tw.schedule(3);
    auto id3 = tw.schedule(3);
    EXPECT_EQ(tw.active_count(), 3u);

    // Reschedule id3 (last scheduled)
    tw.reschedule(id3, 8);
    EXPECT_EQ(tw.active_count(), 3u);

    // Tick past deadline 3 — id1 and id2 should fire
    for (int i = 0; i < 4; ++i)
        tw.tick();
    EXPECT_EQ(expired.size(), 2u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_TRUE(expired.contains(id2));
    EXPECT_FALSE(expired.contains(id3));

    // Tick until id3 fires
    for (int i = 0; i < 6; ++i)
        tw.tick();
    EXPECT_TRUE(expired.contains(id3));
    EXPECT_EQ(tw.active_count(), 0u);
}
