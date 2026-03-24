// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/scoped_logger.hpp>
#include <apex/shared/adapters/kafka/consumer_payload_pool.hpp>

namespace
{
const apex::core::ScopedLogger& s_logger()
{
    static const apex::core::ScopedLogger instance{"ConsumerPayloadPool", apex::core::ScopedLogger::NO_CORE, "app"};
    return instance;
}
} // namespace

namespace apex::shared::adapters::kafka
{

ConsumerPayloadPool::ConsumerPayloadPool(size_t initial_count, size_t buffer_size, size_t max_count)
    : buffer_size_(buffer_size)
    , max_count_(max_count)
{
    // 사전 할당
    free_list_.reserve(initial_count);
    for (size_t i = 0; i < initial_count; ++i)
    {
        free_list_.push_back(create_buffer());
    }
    total_created_ = initial_count;

    s_logger().debug("pre-allocated {} buffers ({}B each, max={})", initial_count, buffer_size, max_count);
}

ConsumerPayloadPool::PayloadPtr ConsumerPayloadPool::acquire(std::span<const uint8_t> payload)
{
    std::unique_ptr<PayloadBuffer> buf;

    {
        std::lock_guard lock(mutex_);
        ++acquire_count_;

        if (!free_list_.empty())
        {
            // 풀에서 가져오기
            buf = std::move(free_list_.back());
            free_list_.pop_back();
        }
        else if (max_count_ == 0 || total_created_ < max_count_)
        {
            // 풀 고갈 — 새 버퍼 생성 (max 이내)
            buf = create_buffer();
            ++total_created_;
            ++fallback_alloc_count_;
        }
        else
        {
            // max 도달 — 힙 fallback (풀 외부)
            ++fallback_alloc_count_;
        }

        ++in_use_;
        if (in_use_ > peak_in_use_)
        {
            peak_in_use_ = in_use_;
        }
    }

    if (!buf)
    {
        // max 도달 시 풀 관리 밖의 순수 힙 할당
        buf = create_buffer();
    }

    // payload 복사
    buf->data.assign(payload.begin(), payload.end());

    // custom deleter: 소멸 시 풀로 반환
    PayloadBuffer* raw = buf.release();
    return PayloadPtr(raw, [this](PayloadBuffer* p) { release(p); });
}

void ConsumerPayloadPool::release(PayloadBuffer* buf)
{
    if (!buf)
        return;

    // 버퍼 내용 클리어 (메모리는 유지, capacity 보존)
    buf->data.clear();

    std::lock_guard lock(mutex_);
    assert(in_use_ > 0 && "ConsumerPayloadPool::release() called with in_use_ == 0");
    --in_use_;

    // max 이내면 풀로 반환, 초과면 삭제
    if (max_count_ == 0 || free_list_.size() + in_use_ < max_count_)
    {
        free_list_.push_back(std::unique_ptr<PayloadBuffer>(buf));
    }
    else
    {
        delete buf;
        if (total_created_ > 0)
            --total_created_;
    }
}

std::unique_ptr<ConsumerPayloadPool::PayloadBuffer> ConsumerPayloadPool::create_buffer() const
{
    auto buf = std::make_unique<PayloadBuffer>();
    buf->data.reserve(buffer_size_);
    return buf;
}

size_t ConsumerPayloadPool::free_count() const noexcept
{
    std::lock_guard lock(mutex_);
    return free_list_.size();
}

size_t ConsumerPayloadPool::in_use_count() const noexcept
{
    std::lock_guard lock(mutex_);
    return in_use_;
}

size_t ConsumerPayloadPool::total_created() const noexcept
{
    std::lock_guard lock(mutex_);
    return total_created_;
}

uint64_t ConsumerPayloadPool::fallback_alloc_count() const noexcept
{
    std::lock_guard lock(mutex_);
    return fallback_alloc_count_;
}

uint64_t ConsumerPayloadPool::acquire_count() const noexcept
{
    std::lock_guard lock(mutex_);
    return acquire_count_;
}

size_t ConsumerPayloadPool::peak_in_use() const noexcept
{
    std::lock_guard lock(mutex_);
    return peak_in_use_;
}

} // namespace apex::shared::adapters::kafka
