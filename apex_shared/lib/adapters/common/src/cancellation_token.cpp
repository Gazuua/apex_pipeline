// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/cancellation_token.hpp>

namespace apex::shared::adapters
{

#ifndef NDEBUG
void CancellationToken::assert_owner_thread()
{
    auto current = std::this_thread::get_id();
    if (owner_thread_ == std::thread::id{})
        owner_thread_ = current;
    else
        assert(owner_thread_ == current && "CancellationToken accessed from wrong thread");
}
#endif

boost::asio::cancellation_slot CancellationToken::new_slot()
{
    assert_owner_thread();
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = slots_.emplace_back(std::make_unique<Slot>());
    return slot->signal.slot();
}

void CancellationToken::cancel_all()
{
    assert_owner_thread();
    for (auto& s : slots_)
        s->signal.emit(boost::asio::cancellation_type::terminal);
}

void CancellationToken::on_complete()
{
    assert_owner_thread();
    auto prev = outstanding_.fetch_sub(1, std::memory_order_release);
    assert(prev > 0 && "on_complete called more times than new_slot");
}

uint32_t CancellationToken::outstanding() const noexcept
{
    return outstanding_.load(std::memory_order_acquire);
}

} // namespace apex::shared::adapters
