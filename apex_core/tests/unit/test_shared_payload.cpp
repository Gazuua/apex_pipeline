#include <apex/core/shared_payload.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

using namespace apex::core;

struct TestPayload : SharedPayload {
    int value{0};
    explicit TestPayload(int v) : value(v) {}
};

TEST(SharedPayload, SingleOwnerRelease) {
    auto* p = new TestPayload(42);
    p->add_ref();
    EXPECT_EQ(p->refcount(), 1u);
    EXPECT_EQ(p->value, 42);

    p->release();  // refcount → 0 → delete
    // No leak (ASAN will catch)
}

TEST(SharedPayload, MultipleOwners) {
    auto* p = new TestPayload(99);
    p->add_ref();
    p->add_ref();
    p->add_ref();
    EXPECT_EQ(p->refcount(), 3u);

    p->release();
    EXPECT_EQ(p->refcount(), 2u);
    p->release();
    EXPECT_EQ(p->refcount(), 1u);
    p->release();  // delete
}

TEST(SharedPayload, BroadcastPattern) {
    // 4코어 브로드캐스트: refcount = 3 (자기 코어 제외)
    auto* p = new TestPayload(7);
    p->set_refcount(3);

    p->release();  // core 1
    p->release();  // core 2
    EXPECT_EQ(p->refcount(), 1u);
    p->release();  // core 3 → delete
}
