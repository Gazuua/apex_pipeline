#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace apex::core {

/// Hierarchical timing wheel for O(1) timeout management.
/// Designed for per-core use (no thread synchronization).
///
/// Usage with Asio: a single steady_timer drives tick() at fixed intervals.
/// Sessions are added with a timeout. On each tick, expired entries fire callbacks.
///
/// Typical configuration: 512 slots, 1-second tick = ~8.5 minute max timeout.
class TimingWheel {
public:
    /// EntryId 0 is never issued by schedule() and serves as an invalid/sentinel value.
    using EntryId = uint64_t;
    using Callback = std::function<void(EntryId)>;

    /// @param num_slots Number of slots in the wheel (rounded up to power of 2).
    /// @param on_expire Callback invoked for each expired entry.
    TimingWheel(size_t num_slots, Callback on_expire);

    ~TimingWheel();

    // Non-copyable, non-movable
    TimingWheel(const TimingWheel&) = delete;
    TimingWheel& operator=(const TimingWheel&) = delete;
    TimingWheel(TimingWheel&&) = delete;
    TimingWheel& operator=(TimingWheel&&) = delete;

    /// Add an entry that expires after `ticks_from_now` ticks.
    /// @param ticks_from_now Number of ticks until expiration. 0 means "expire on
    /// the next tick() call". Note: if called from within a tick() callback,
    /// schedule(0) targets the current slot which has already been collected,
    /// so the entry will expire after a full wheel revolution (num_slots ticks).
    /// Use ticks_from_now >= 1 from within callbacks to avoid this behavior.
    /// @note 매 호출 시 힙 할당 발생. 빈번한 타임아웃 갱신은 reschedule()을 사용할 것.
    /// @return EntryId for later cancel/reschedule.
    [[nodiscard]] EntryId schedule(uint32_t ticks_from_now);

    /// Cancel a previously scheduled entry. O(1).
    /// No-op if already expired or cancelled.
    void cancel(EntryId id);

    /// Reschedule an existing entry to expire after `ticks_from_now` ticks.
    /// Equivalent to cancel + schedule but reuses the same id.
    void reschedule(EntryId id, uint32_t ticks_from_now);

    /// Advance the wheel by one tick. Fires on_expire for all entries in the current slot.
    /// Called by Asio steady_timer at fixed intervals.
    void tick();

    /// Number of currently active (non-expired, non-cancelled) entries.
    [[nodiscard]] size_t active_count() const noexcept;

    /// Current tick position in the wheel.
    [[nodiscard]] uint64_t current_tick() const noexcept;

private:
    struct Entry {
        EntryId id;
        uint64_t deadline_tick;
        bool cancelled{false};
        Entry* next{nullptr};
        Entry* prev{nullptr};
    };

    struct Slot {
        Entry* head{nullptr};
    };

    void insert_entry(Entry* entry, size_t slot_idx);
    void remove_entry(Entry* entry, size_t slot_idx);
    uint64_t compute_deadline(uint32_t ticks_from_now) const;

    std::vector<Slot> slots_;
    size_t num_slots_;
    size_t mask_;
    uint64_t current_tick_{0};
    EntryId next_id_{1};
    Callback on_expire_;

    // Entry storage (pool-friendly)
    std::vector<Entry*> entries_;  // indexed by id for O(1) lookup
    std::vector<EntryId> free_ids_;  // free-list for id reuse
    size_t active_count_{0};  // 현재 활성 엔트리 수 (O(1) 조회용)
    std::vector<Entry*> expired_buf_;  // tick()에서 재사용하는 만료 엔트리 버퍼
};

} // namespace apex::core
