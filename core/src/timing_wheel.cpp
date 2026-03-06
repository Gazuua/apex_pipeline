#include <apex/core/timing_wheel.hpp>
#include <apex/core/detail/math_utils.hpp>
#include <algorithm>
#include <cassert>

namespace apex::core {

TimingWheel::TimingWheel(size_t num_slots, Callback on_expire)
    : num_slots_(detail::next_power_of_2(num_slots < 1 ? 1 : num_slots))
    , mask_(num_slots_ - 1)
    , on_expire_(std::move(on_expire))
{
    slots_.resize(num_slots_);
}

TimingWheel::~TimingWheel() {
    for (auto* entry : entries_) {
        delete entry;
    }
}

void TimingWheel::insert_entry(Entry* entry, size_t slot_idx) {
    entry->prev = nullptr;
    entry->next = slots_[slot_idx].head;
    if (slots_[slot_idx].head) {
        slots_[slot_idx].head->prev = entry;
    }
    slots_[slot_idx].head = entry;
}

void TimingWheel::remove_entry(Entry* entry, size_t slot_idx) {
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        slots_[slot_idx].head = entry->next;
    }
    if (entry->next) {
        entry->next->prev = entry->prev;
    }
    entry->prev = nullptr;
    entry->next = nullptr;
}

TimingWheel::EntryId TimingWheel::schedule(uint32_t ticks_from_now) {
    EntryId id = next_id_++;

    auto* entry = new Entry();
    entry->id = id;
    entry->deadline_tick = (ticks_from_now == 0)
        ? current_tick_
        : current_tick_ + ticks_from_now - 1;
    entry->cancelled = false;

    size_t slot_idx = entry->deadline_tick & mask_;
    insert_entry(entry, slot_idx);

    if (id >= entries_.size()) {
        entries_.resize(id + 1, nullptr);
    }
    entries_[id] = entry;

    return id;
}

void TimingWheel::cancel(EntryId id) {
    if (id >= entries_.size() || !entries_[id]) return;

    Entry* entry = entries_[id];
    if (entry->cancelled) return;

    entry->cancelled = true;
    size_t slot_idx = entry->deadline_tick & mask_;
    remove_entry(entry, slot_idx);

    delete entry;
    entries_[id] = nullptr;
}

void TimingWheel::reschedule(EntryId id, uint32_t ticks_from_now) {
    if (id >= entries_.size() || !entries_[id]) return;

    Entry* entry = entries_[id];
    if (entry->cancelled) return;

    size_t old_slot = entry->deadline_tick & mask_;
    remove_entry(entry, old_slot);

    entry->deadline_tick = (ticks_from_now == 0)
        ? current_tick_
        : current_tick_ + ticks_from_now - 1;
    size_t new_slot = entry->deadline_tick & mask_;
    insert_entry(entry, new_slot);
}

void TimingWheel::tick() {
    size_t slot_idx = current_tick_ & mask_;
    Entry* entry = slots_[slot_idx].head;

    while (entry) {
        Entry* next = entry->next;

        if (!entry->cancelled && entry->deadline_tick == current_tick_) {
            remove_entry(entry, slot_idx);
            on_expire_(entry->id);
            entries_[entry->id] = nullptr;
            delete entry;
        }

        entry = next;
    }

    ++current_tick_;
}

size_t TimingWheel::active_count() const noexcept {
    size_t count = 0;
    for (const auto* entry : entries_) {
        if (entry && !entry->cancelled) ++count;
    }
    return count;
}

uint64_t TimingWheel::current_tick() const noexcept {
    return current_tick_;
}

} // namespace apex::core
