#include <apex/gateway/gateway_config_parser.hpp>

#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

namespace apex::gateway {

apex::core::Result<GatewayConfig>
parse_gateway_config(std::string_view path) {
    try {
        auto tbl = toml::parse_file(path);
        GatewayConfig cfg;

        // [server]
        if (auto server = tbl["server"]; server) {
            cfg.ws_port = static_cast<uint16_t>(
                server["ws_port"].value_or(int64_t{8443}));
            cfg.num_cores = static_cast<uint32_t>(
                server["num_cores"].value_or(int64_t{1}));
        }

        // [tls]
        if (auto tls = tbl["tls"]; tls) {
            cfg.tls.cert_file = tls["cert_file"]
                .value_or(std::string{});
            cfg.tls.key_file = tls["key_file"]
                .value_or(std::string{});
            cfg.tls.ca_file = tls["ca_file"]
                .value_or(std::string{});
        }

        // [jwt]
        if (auto jwt = tbl["jwt"]; jwt) {
            cfg.jwt.secret = jwt["secret"]
                .value_or(std::string{});
            cfg.jwt.algorithm = jwt["algorithm"]
                .value_or(std::string{"HS256"});
            cfg.jwt.clock_skew = std::chrono::seconds{
                jwt["clock_skew_seconds"].value_or(int64_t{30})};

            if (auto arr = jwt["sensitive_msg_ids"].as_array()) {
                for (auto& elem : *arr) {
                    cfg.jwt.sensitive_msg_ids.push_back(
                        static_cast<uint32_t>(elem.value_or(int64_t{0})));
                }
            }
        }

        // [[routes]]
        if (auto routes = tbl["routes"].as_array()) {
            for (auto& entry : *routes) {
                if (auto* t = entry.as_table()) {
                    RouteEntry r;
                    r.range_begin = static_cast<uint32_t>(
                        (*t)["range_begin"].value_or(int64_t{0}));
                    r.range_end = static_cast<uint32_t>(
                        (*t)["range_end"].value_or(int64_t{0}));
                    r.kafka_topic = (*t)["kafka_topic"]
                        .value_or(std::string{});
                    cfg.routes.push_back(std::move(r));
                }
            }
        }

        // [kafka]
        if (auto kafka = tbl["kafka"]; kafka) {
            cfg.kafka_brokers = kafka["brokers"]
                .value_or(std::string{"localhost:9092"});
            cfg.kafka_consumer_group = kafka["consumer_group"]
                .value_or(std::string{"gateway"});
            cfg.kafka_response_topic = kafka["response_topic"]
                .value_or(std::string{"gateway.responses"});
        }

        // [redis.pubsub]
        if (auto rpub = tbl["redis"]["pubsub"]; rpub) {
            cfg.redis_pubsub_host = rpub["host"]
                .value_or(std::string{"localhost"});
            cfg.redis_pubsub_port = static_cast<uint16_t>(
                rpub["port"].value_or(int64_t{6379}));
            cfg.redis_pubsub_password = rpub["password"]
                .value_or(std::string{});
        }

        // [redis.auth]
        if (auto rauth = tbl["redis"]["auth"]; rauth) {
            cfg.redis_auth_host = rauth["host"]
                .value_or(std::string{"localhost"});
            cfg.redis_auth_port = static_cast<uint16_t>(
                rauth["port"].value_or(int64_t{6379}));
            cfg.redis_auth_password = rauth["password"]
                .value_or(std::string{});
        }

        // [timeouts]
        if (auto timeouts = tbl["timeouts"]; timeouts) {
            cfg.request_timeout = std::chrono::milliseconds{
                timeouts["request_timeout_ms"].value_or(int64_t{5000})};
            cfg.max_pending_per_core = static_cast<size_t>(
                timeouts["max_pending_per_core"].value_or(int64_t{65536}));
        }

        // [rate_limit.ip]
        if (auto rl_ip = tbl["rate_limit"]["ip"]; rl_ip) {
            cfg.rate_limit.ip.total_limit = static_cast<uint32_t>(
                rl_ip["total_limit"].value_or(int64_t{1000}));
            cfg.rate_limit.ip.window_size_seconds = static_cast<uint32_t>(
                rl_ip["window_size_seconds"].value_or(int64_t{60}));
            cfg.rate_limit.ip.max_entries = static_cast<uint32_t>(
                rl_ip["max_entries"].value_or(int64_t{65536}));
            cfg.rate_limit.ip.ttl_multiplier = static_cast<uint32_t>(
                rl_ip["ttl_multiplier"].value_or(int64_t{2}));
        }

        // [rate_limit.user]
        if (auto rl_user = tbl["rate_limit"]["user"]; rl_user) {
            cfg.rate_limit.user.default_limit = static_cast<uint32_t>(
                rl_user["default_limit"].value_or(int64_t{100}));
            cfg.rate_limit.user.window_size_seconds = static_cast<uint32_t>(
                rl_user["window_size_seconds"].value_or(int64_t{60}));
        }

        // [rate_limit.endpoint]
        if (auto rl_ep = tbl["rate_limit"]["endpoint"]; rl_ep) {
            cfg.rate_limit.endpoint.default_limit = static_cast<uint32_t>(
                rl_ep["default_limit"].value_or(int64_t{60}));
            cfg.rate_limit.endpoint.window_size_seconds = static_cast<uint32_t>(
                rl_ep["window_size_seconds"].value_or(int64_t{60}));

            // [rate_limit.endpoint.overrides] — msg_id = limit
            if (auto* ot = rl_ep["overrides"].as_table()) {
                for (auto& [key, val] : *ot) {
                    try {
                        auto msg_id = static_cast<uint32_t>(std::stoul(std::string(key.str())));
                        auto limit = static_cast<uint32_t>(val.value_or(int64_t{0}));
                        cfg.rate_limit.endpoint.overrides.emplace_back(msg_id, limit);
                    } catch (const std::exception& e) {
                        spdlog::warn("Invalid endpoint override key '{}': {}",
                                     key.str(), e.what());
                    }
                }
            }
        }

        return cfg;

    } catch (const toml::parse_error& e) {
        spdlog::error("Gateway config parse error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ConfigParseFailed);
    }
}

} // namespace apex::gateway
