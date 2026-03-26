// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/admin_http_server.hpp>

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

namespace apex::core
{

namespace http = boost::beast::http;

namespace
{

/// Convert spdlog level to std::string (spdlog::string_view_t may not be std::string_view).
std::string level_to_string(spdlog::level::level_enum level)
{
    auto sv = spdlog::level::to_string_view(level);
    return {sv.data(), sv.size()};
}

} // anonymous namespace

AdminHttpServer::AdminHttpServer()
{
    logger_ = ScopedLogger{"AdminHttpServer", ScopedLogger::NO_CORE};
}

HttpResponse AdminHttpServer::handle_request(http::verb method, std::string_view target)
{
    // Split target into path and query string
    auto qpos = target.find('?');
    auto path = (qpos != std::string_view::npos) ? target.substr(0, qpos) : target;
    auto query = (qpos != std::string_view::npos) ? target.substr(qpos + 1) : std::string_view{};

    if (path == "/admin/log-level")
    {
        if (method == http::verb::get)
        {
            return handle_log_level_get(query);
        }
        if (method == http::verb::post)
        {
            return handle_log_level_post(query);
        }
        return {405, "application/json", R"({"error":"method not allowed, use GET or POST"})"};
    }

    return {404, "application/json", R"({"error":"not found"})"};
}

HttpResponse AdminHttpServer::handle_log_level_get(std::string_view query) const
{
    auto logger_name = parse_query_param(query, "logger");

    // Single logger query
    if (!logger_name.empty())
    {
        auto logger = spdlog::get(logger_name);
        if (!logger)
        {
            return {400, "application/json", R"({"error":"unknown logger: )" + logger_name + R"("})"};
        }
        auto level_str = level_to_string(logger->level());
        return {200, "application/json", R"({"logger":")" + logger_name + R"(","level":")" + level_str + R"("})"};
    }

    // All loggers
    std::string body = "{";
    bool first = true;
    for (const auto& name : {"apex", "app"})
    {
        auto logger = spdlog::get(name);
        if (logger)
        {
            if (!first)
                body += ",";
            body += R"(")" + std::string(name) + R"(":")" + level_to_string(logger->level()) + R"(")";
            first = false;
        }
    }
    body += "}";
    return {200, "application/json", body};
}

HttpResponse AdminHttpServer::handle_log_level_post(std::string_view query)
{
    auto logger_name = parse_query_param(query, "logger");
    auto level_str = parse_query_param(query, "level");

    if (logger_name.empty())
    {
        return {400, "application/json", R"json({"error":"missing 'logger' parameter (apex or app)"})json"};
    }

    if (level_str.empty())
    {
        return {400, "application/json",
                R"json({"error":"missing 'level' parameter (trace/debug/info/warn/error/critical)"})json"};
    }

    auto logger = spdlog::get(logger_name);
    if (!logger)
    {
        return {400, "application/json", R"({"error":"unknown logger: )" + logger_name + R"("})"};
    }

    auto new_level = spdlog::level::from_str(level_str);
    if (new_level == spdlog::level::off && level_str != "off")
    {
        return {400, "application/json", R"({"error":"invalid level: )" + level_str + R"("})"};
    }

    auto previous = level_to_string(logger->level());
    logger->set_level(new_level);

    logger_.info("log level changed: {}={} (was {})", logger_name, level_str, previous);

    return {200, "application/json",
            R"({"logger":")" + logger_name + R"(","level":")" + level_str + R"(","previous":")" + previous + R"("})"};
}

std::string AdminHttpServer::parse_query_param(std::string_view query, std::string_view key)
{
    // Simple query string parser: key=value&key2=value2
    std::string search = std::string(key) + "=";
    auto pos = query.find(search);
    if (pos == std::string_view::npos)
        return {};

    // Check it's at start or preceded by '&'
    if (pos > 0 && query[pos - 1] != '&')
        return {};

    auto value_start = pos + search.size();
    auto value_end = query.find('&', value_start);
    if (value_end == std::string_view::npos)
        return std::string(query.substr(value_start));
    return std::string(query.substr(value_start, value_end - value_start));
}

} // namespace apex::core
