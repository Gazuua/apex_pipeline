#include <apex/gateway/pending_requests.hpp>

#include <spdlog/spdlog.h>

namespace apex::gateway
{

PendingRequestsMap::PendingRequestsMap(size_t max_entries, std::chrono::milliseconds timeout, NowFn now_fn)
    : max_entries_(max_entries)
    , timeout_(timeout)
    , now_fn_(std::move(now_fn))
{}

apex::core::Result<void> PendingRequestsMap::insert(uint64_t corr_id, apex::core::SessionId session_id,
                                                    uint32_t original_msg_id)
{
    if (map_.size() >= max_entries_)
    {
        spdlog::warn("PendingRequestsMap full ({}/{})", map_.size(), max_entries_);
        return apex::core::error(apex::core::ErrorCode::PendingMapFull);
    }

    map_.emplace(corr_id, PendingEntry{
                              .session_id = session_id,
                              .original_msg_id = original_msg_id,
                              .deadline = now_fn_() + timeout_,
                          });

    return apex::core::ok();
}

std::optional<PendingRequestsMap::PendingEntry> PendingRequestsMap::extract(uint64_t corr_id)
{
    auto it = map_.find(corr_id);
    if (it == map_.end())
        return std::nullopt;

    auto entry = std::move(it->second);
    map_.erase(it);
    return entry;
}

void PendingRequestsMap::sweep_expired(std::function<void(uint64_t, const PendingEntry&)> callback)
{
    auto now = now_fn_();
    for (auto it = map_.begin(); it != map_.end();)
    {
        if (it->second.deadline <= now)
        {
            if (callback)
                callback(it->first, it->second);
            it = map_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} // namespace apex::gateway
