#include <apex/auth_svc/session_store.hpp>

#include <apex/core/core_engine.hpp>
#include <spdlog/spdlog.h>

#include <format>

namespace apex::auth_svc {

SessionStore::SessionStore(
    apex::shared::adapters::redis::RedisAdapter& redis,
    std::string prefix,
    std::string blacklist_prefix,
    std::chrono::seconds ttl)
    : redis_(redis)
    , prefix_(std::move(prefix))
    , blacklist_prefix_(std::move(blacklist_prefix))
    , ttl_(ttl)
{}

boost::asio::awaitable<apex::core::Result<void>>
SessionStore::set(uint64_t user_id, std::string_view session_data) {
    auto key = session_key(user_id);
    auto ttl_sec = static_cast<int>(ttl_.count());

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("SETEX %s %d %s",
                                        key.c_str(), ttl_sec,
                                        std::string(session_data).c_str());

    if (!result.has_value()) {
        spdlog::error("[SessionStore] Failed to set session for user {}", user_id);
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<std::string>>
SessionStore::get(uint64_t user_id) {
    auto key = session_key(user_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("GET %s", key.c_str());

    if (!result.has_value()) {
        co_return apex::core::error(result.error());
    }

    if (result->is_nil()) {
        co_return apex::core::error(apex::core::ErrorCode::AdapterError);
    }

    co_return result->str;
}

boost::asio::awaitable<apex::core::Result<void>>
SessionStore::remove(uint64_t user_id) {
    auto key = session_key(user_id);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("DEL %s", key.c_str());

    if (!result.has_value()) {
        spdlog::error("[SessionStore] Failed to remove session for user {}", user_id);
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<void>>
SessionStore::blacklist_token(std::string_view token_hash,
                              std::chrono::seconds ttl) {
    auto key = blacklist_key(token_hash);
    auto ttl_sec = static_cast<int>(ttl.count());

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("SETEX %s %d %s",
                                        key.c_str(), ttl_sec, "1");

    if (!result.has_value()) {
        spdlog::error("[SessionStore] Failed to blacklist token");
        co_return apex::core::error(result.error());
    }

    co_return apex::core::ok();
}

boost::asio::awaitable<apex::core::Result<bool>>
SessionStore::is_blacklisted(std::string_view token_hash) {
    auto key = blacklist_key(token_hash);

    auto core_id = apex::core::CoreEngine::current_core_id();
    auto& mux = redis_.multiplexer(core_id);
    auto result = co_await mux.command("EXISTS %s", key.c_str());

    if (!result.has_value()) {
        // Conservative: treat Redis error as blacklisted
        co_return true;
    }

    co_return result->integer > 0;
}

std::string SessionStore::session_key(uint64_t user_id) const {
    return std::format("{}{}", prefix_, user_id);
}

std::string SessionStore::blacklist_key(std::string_view token_hash) const {
    return std::format("{}{}", blacklist_prefix_, token_hash);
}

} // namespace apex::auth_svc
