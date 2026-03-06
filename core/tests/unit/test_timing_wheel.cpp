#include <apex/core/timing_wheel.hpp>
#include <gtest/gtest.h>
#include <set>

using namespace apex::core;

TEST(TimingWheel, Construction) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });
    EXPECT_EQ(tw.active_count(), 0u);
    EXPECT_EQ(tw.current_tick(), 0u);
}

TEST(TimingWheel, ScheduleAndExpire) {
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

TEST(TimingWheel, ScheduleAtTickZero) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    (void)tw.schedule(0);
    tw.tick();
    EXPECT_EQ(expired.size(), 1u);
}

TEST(TimingWheel, Cancel) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    tw.tick();
    tw.tick();
    EXPECT_TRUE(expired.empty());
}

TEST(TimingWheel, Reschedule) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(2);
    tw.tick(); // tick 0->1

    tw.reschedule(id, 3);  // now expires at tick 1 + 3 = tick 4

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

TEST(TimingWheel, MultipleEntries_SameSlot) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(5);
    auto id2 = tw.schedule(5);
    auto id3 = tw.schedule(5);

    for (int i = 0; i < 6; ++i) tw.tick(); // deadline=5, fires on tick 5

    EXPECT_EQ(expired.size(), 3u);
    EXPECT_TRUE(expired.contains(id1));
    EXPECT_TRUE(expired.contains(id2));
    EXPECT_TRUE(expired.contains(id3));
}

TEST(TimingWheel, WrapAround) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(8, [&](TimingWheel::EntryId id) { expired.insert(id); });

    for (int i = 0; i < 8; ++i) tw.tick();

    auto id = tw.schedule(3);
    tw.tick(); tw.tick(); tw.tick(); tw.tick(); // deadline=11, fires on tick 11
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

// T4: Double cancel safety - second cancel should be no-op
TEST(TimingWheel, DoubleCancelSafe) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(5);
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    // Second cancel — must not crash or corrupt state
    tw.cancel(id);
    EXPECT_EQ(tw.active_count(), 0u);

    // Tick past deadline — must not fire
    for (int i = 0; i < 8; ++i) tw.tick();
    EXPECT_TRUE(expired.empty());
}

// T4b: Reschedule after expiry — should be safe (no-op or ignored)
TEST(TimingWheel, RescheduleAfterExpiry) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(1);
    tw.tick(); // tick 0->1
    tw.tick(); // tick 1->2 — fires at tick 1
    ASSERT_EQ(expired.size(), 1u);

    // Reschedule an already-expired entry — must not crash
    tw.reschedule(id, 3);

    // Tick some more — should not double-fire
    for (int i = 0; i < 5; ++i) tw.tick();
    EXPECT_EQ(expired.size(), 1u);  // still just the original fire
}

TEST(TimingWheel, MaxValidTimeout) {
    // num_slots=64 (power of 2), max valid ticks_from_now = num_slots - 1 = 63
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id = tw.schedule(63);  // max valid value (num_slots - 1)
    EXPECT_EQ(tw.active_count(), 1u);

    // Tick 63 times: should NOT fire yet (deadline = 63, fires when tick reaches 63)
    for (int i = 0; i < 63; ++i) tw.tick();
    EXPECT_TRUE(expired.empty());

    // One more tick should fire (tick passes the deadline)
    tw.tick();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, CallbackCanScheduleNewEntry) {
    size_t expire_count = 0;
    TimingWheel::EntryId second_id = 0;

    TimingWheel tw(64, [&](TimingWheel::EntryId) {
        ++expire_count;
        if (expire_count == 1) {
            // Schedule a new entry from within the callback.
            // At this point current_tick_ is still 1 (pre-increment in tick()).
            // compute_deadline(2) = current_tick_ + 2 = 1 + 2 = 3
            second_id = tw.schedule(2);
        }
    });

    (void)tw.schedule(1);  // deadline = 0 + 1 = 1
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

TEST(TimingWheel, LargeNumberOfEntries) {
    size_t expire_count = 0;
    TimingWheel tw(256, [&](TimingWheel::EntryId) { ++expire_count; });

    for (int i = 1; i <= 1000; ++i) {
        (void)tw.schedule(i % 200 + 1);
    }
    EXPECT_EQ(tw.active_count(), 1000u);

    for (int i = 0; i < 250; ++i) tw.tick();
    EXPECT_EQ(expire_count, 1000u);
    EXPECT_EQ(tw.active_count(), 0u);
}
