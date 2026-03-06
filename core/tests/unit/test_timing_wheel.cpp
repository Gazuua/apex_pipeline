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

    tw.tick(); // tick 1
    tw.tick(); // tick 2
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3 — should fire
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
    EXPECT_EQ(tw.active_count(), 0u);
}

TEST(TimingWheel, ScheduleAtTickZero) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    tw.schedule(0);
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
    tw.tick(); // tick 1

    tw.reschedule(id, 3);  // now expires at tick 1 + 3 = tick 4

    tw.tick(); // tick 2
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 3
    EXPECT_TRUE(expired.empty());

    tw.tick(); // tick 4 — should fire
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

TEST(TimingWheel, MultipleEntries_SameSlot) {
    std::set<TimingWheel::EntryId> expired;
    TimingWheel tw(64, [&](TimingWheel::EntryId id) { expired.insert(id); });

    auto id1 = tw.schedule(5);
    auto id2 = tw.schedule(5);
    auto id3 = tw.schedule(5);

    for (int i = 0; i < 5; ++i) tw.tick();

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
    tw.tick(); tw.tick(); tw.tick();
    EXPECT_EQ(expired.size(), 1u);
    EXPECT_TRUE(expired.contains(id));
}

TEST(TimingWheel, LargeNumberOfEntries) {
    size_t expire_count = 0;
    TimingWheel tw(256, [&](TimingWheel::EntryId) { ++expire_count; });

    for (int i = 1; i <= 1000; ++i) {
        tw.schedule(i % 200 + 1);
    }
    EXPECT_EQ(tw.active_count(), 1000u);

    for (int i = 0; i < 250; ++i) tw.tick();
    EXPECT_EQ(expire_count, 1000u);
    EXPECT_EQ(tw.active_count(), 0u);
}
