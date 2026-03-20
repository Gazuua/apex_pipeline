// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/auth_svc/session_store.hpp>

#include <apex/core/core_engine.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>

namespace apex::auth_svc
{

namespace
{

/// Validate token hash: must be hex string (SHA-256 = 64 chars).
/// Prevents Redis key injection via crafted token_hash values.
bool is_valid_token_hash(std::string_view hash) noexcept
{
    if (hash.empty() || hash.size() > 128)
        return false;
    return std::ranges::all_of(
        hash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
}

} // anonymous namespace

SessionStore::SessionStore(apex::shared::adapters::redis::RedisAdapter& redis, std::string prefix,
                           std::string blacklist_prefix, std::chrono::seconds ttl)
    : redis_(redis)
    , prefix_(std::move(prefix))
    , blacklist_prefix_(std::move(blacklist_prefix))
    , ttl_(ttl)
{}

boost::asio::awaitable<apex::core::Result<void>> SessionStore::set(uint64_t user_id, std::string_view session_data)
{
    auto key = session_key(user_id);
    auto ttl_sec = static_cast<int>(ttl_.count());

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("SETEX %s %d %s", key.c_str(), ttl_sec, std::string(session_data).c_str());

    if (!result.has_value())
    {
        spdlog::error("[SessionStore] Failed to set session for user {}", user_id);
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<std::string>> SessionStore::get(uint64_t user_id)
{
    auto key = session_key(user_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("GET %s", key.c_str());

    if (!result.has_value())
    {
        co_return apex::core::error(result.error());
    }

    if (result->is_nil())
    {
        co_return apex::core::error(apex::core::ErrorCode::AdapterError);
    }

    co_return result->str;
}

boost::asio::awaitable<apex::core::Result<void>> SessionStore::remove(uint64_t user_id)
{
    auto key = session_key(user_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("DEL %s", key.c_str());

    if (!result.has_value())
    {
        spdlog::error("[SessionStore] Failed to remove session for user {}", user_id);
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> SessionStore::set_user_session_id(uint64_t user_id,
                                                                                   uint64_t session_id)
{
    auto key = std::format("session:user:{}", user_id);
    auto value = std::to_string(session_id);
    auto ttl_sec = static_cast<int>(ttl_.count());

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("SETEX %s %d %s", key.c_str(), ttl_sec, value.c_str());
    if (!result.has_value())
    {
        spdlog::error("[SessionStore] Failed to set user session_id for user {}", user_id);
        co_return apex::core::error(result.error());
    }
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> SessionStore::remove_user_session_id(uint64_t user_id)
{
    auto key = std::format("session:user:{}", user_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("DEL %s", key.c_str());
    if (!result.has_value())
    {
        spdlog::error("[SessionStore] Failed to remove user session_id for user {}", user_id);
        co_return apex::core::error(result.error());
    }
    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>> SessionStore::blacklist_token(std::string_view token_hash,
                                                                               std::chrono::seconds ttl)
{
    if (!is_valid_token_hash(token_hash))
    {
        spdlog::warn("[SessionStore] Invalid token_hash format, rejecting blacklist");
        co_return apex::core::error(apex::core::ErrorCode::InvalidMessage);
    }

    auto key = blacklist_key(token_hash);
    auto ttl_sec = static_cast<int>(ttl.count());

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("SETEX %s %d %s", key.c_str(), ttl_sec, "1");

    if (!result.has_value())
    {
        spdlog::error("[SessionStore] Failed to blacklist token");
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<bool>> SessionStore::is_blacklisted(std::string_view token_hash)
{
    if (!is_valid_token_hash(token_hash))
    {
        spdlog::warn("[SessionStore] Invalid token_hash format, rejecting as blacklisted");
        co_return true; // Reject invalid hash (conservative)
    }

    auto key = blacklist_key(token_hash);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("EXISTS %s", key.c_str());

    if (!result.has_value())
    {
        // Conservative: treat Redis error as blacklisted
        co_return true;
    }

    co_return result->integer > 0;
}

std::string SessionStore::session_key(uint64_t user_id) const
{
    return std::format("{}{}", prefix_, user_id);
}

std::string SessionStore::blacklist_key(std::string_view token_hash) const
{
    return std::format("{}{}", blacklist_prefix_, token_hash);
}

} // namespace apex::auth_svc
