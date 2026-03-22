// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/pg/pg_adapter.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/use_awaitable.hpp>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace apex::shared::adapters::pg
{

PgAdapter::PgAdapter(PgAdapterConfig config)
    : config_(std::move(config))
{}

PgAdapter::~PgAdapter()
{
    // Safe cleanup: pool destructors handle connection cleanup
    pools_.clear();
}

void PgAdapter::do_init(apex::core::CoreEngine& engine)
{
    // Validate configuration before creating pools
    if (config_.connection_string.empty())
    {
        spdlog::error("PgAdapter: connection_string is empty — aborting adapter init");
        throw std::runtime_error("PgAdapter: connection_string is empty");
    }
    if (config_.pool_size_per_core == 0)
    {
        spdlog::error("PgAdapter: pool_size_per_core is 0 — aborting adapter init");
        throw std::runtime_error("PgAdapter: pool_size_per_core must be > 0");
    }

    pools_.reserve(engine.core_count());
    for (uint32_t i = 0; i < engine.core_count(); ++i)
    {
        pools_.push_back(std::make_unique<PgPool>(engine.io_context(i), config_));
    }

    // Mask password in connection string for logging
    auto masked = config_.connection_string;
    auto pos = masked.find("password=");
    if (pos != std::string::npos)
    {
        auto end = masked.find(' ', pos);
        if (end == std::string::npos)
            end = masked.size();
        masked.replace(pos, end - pos, "password=***");
    }
    spdlog::info("PgAdapter initialized: {} cores, conninfo={}", engine.core_count(), masked);
}

void PgAdapter::do_drain()
{
    // AdapterBase가 state를 DRAINING으로 설정한 후 호출됨.
    // PgPool 추가 작업 불필요 — in-flight 쿼리는 자연 완료.
    spdlog::info("PgAdapter: drain started");
}

void PgAdapter::do_close()
{
    for (auto& pool : pools_)
    {
        pool->close_all();
    }
    pools_.clear();
    spdlog::info("PgAdapter: closed");
}

PgPool& PgAdapter::current_pool()
{
    auto core_id = apex::core::CoreEngine::current_core_id();
    assert(core_id < pools_.size() && "PgAdapter: invalid core_id");
    return *pools_[core_id];
}

boost::asio::awaitable<apex::core::Result<PgResult>> PgAdapter::query(std::string_view sql)
{
    auto conn_result = co_await current_pool().acquire_connected();
    if (!conn_result.has_value())
    {
        co_return std::unexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto result = co_await conn->query_async(sql);
    if (result.has_value())
    {
        current_pool().release(std::move(conn));
    }
    else
    {
        current_pool().discard(std::move(conn));
    }
    co_return result;
}

boost::asio::awaitable<apex::core::Result<PgResult>> PgAdapter::query(std::string_view sql,
                                                                      std::span<const std::string> params)
{
    auto conn_result = co_await current_pool().acquire_connected();
    if (!conn_result.has_value())
    {
        co_return std::unexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto result = co_await conn->query_params_async(sql, params);
    if (result.has_value())
    {
        current_pool().release(std::move(conn));
    }
    else
    {
        current_pool().discard(std::move(conn));
    }
    co_return result;
}

boost::asio::awaitable<apex::core::Result<int>> PgAdapter::execute(std::string_view sql)
{
    auto conn_result = co_await current_pool().acquire_connected();
    if (!conn_result.has_value())
    {
        co_return std::unexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto result = co_await conn->execute_async(sql);
    if (result.has_value())
    {
        current_pool().release(std::move(conn));
    }
    else
    {
        current_pool().discard(std::move(conn));
    }
    co_return result;
}

boost::asio::awaitable<apex::core::Result<int>> PgAdapter::execute(std::string_view sql,
                                                                   std::span<const std::string> params)
{
    auto conn_result = co_await current_pool().acquire_connected();
    if (!conn_result.has_value())
    {
        co_return std::unexpected(conn_result.error());
    }
    auto conn = std::move(conn_result.value());
    auto result = co_await conn->execute_params_async(sql, params);
    if (result.has_value())
    {
        current_pool().release(std::move(conn));
    }
    else
    {
        current_pool().discard(std::move(conn));
    }
    co_return result;
}

size_t PgAdapter::active_connections() const noexcept
{
    size_t total = 0;
    for (const auto& pool : pools_)
    {
        total += pool->active_count();
    }
    return total;
}

size_t PgAdapter::idle_connections() const noexcept
{
    size_t total = 0;
    for (const auto& pool : pools_)
    {
        total += pool->idle_count();
    }
    return total;
}

PgPool& PgAdapter::pool(uint32_t core_id)
{
    assert(core_id < pools_.size() && "core_id out of range");
    return *pools_[core_id];
}

} // namespace apex::shared::adapters::pg
