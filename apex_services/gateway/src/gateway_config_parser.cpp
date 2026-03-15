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
        if (auto server = tbl["server"].as_table()) {
            cfg.ws_port = static_cast<uint16_t>(
                server->get("ws_port")->value_or(int64_t{8443}));
            cfg.num_cores = static_cast<uint32_t>(
                server->get("num_cores")->value_or(int64_t{1}));
        }

        // [tls]
        if (auto tls = tbl["tls"].as_table()) {
            cfg.tls.cert_file = tls->get("cert_file")
                ->value_or(std::string{});
            cfg.tls.key_file = tls->get("key_file")
                ->value_or(std::string{});
            cfg.tls.ca_file = tls->get("ca_file")
                ->value_or(std::string{});
        }

        // [jwt]
        if (auto jwt = tbl["jwt"].as_table()) {
            cfg.jwt.secret = jwt->get("secret")
                ->value_or(std::string{});
            cfg.jwt.algorithm = jwt->get("algorithm")
                ->value_or(std::string{"HS256"});
            cfg.jwt.clock_skew = std::chrono::seconds{
                jwt->get("clock_skew_seconds")->value_or(int64_t{30})};

            if (auto arr = jwt->get("sensitive_msg_ids")) {
                if (auto* a = arr->as_array()) {
                    for (auto& elem : *a) {
                        cfg.jwt.sensitive_msg_ids.push_back(
                            static_cast<uint32_t>(elem.value_or(int64_t{0})));
                    }
                }
            }
        }

        // [[routes]]
        if (auto routes = tbl["routes"].as_array()) {
            for (auto& entry : *routes) {
                if (auto t = entry.as_table()) {
                    RouteEntry r;
                    r.range_begin = static_cast<uint32_t>(
                        t->get("range_begin")->value_or(int64_t{0}));
                    r.range_end = static_cast<uint32_t>(
                        t->get("range_end")->value_or(int64_t{0}));
                    r.kafka_topic = t->get("kafka_topic")
                        ->value_or(std::string{});
                    cfg.routes.push_back(std::move(r));
                }
            }
        }

        // [kafka]
        if (auto kafka = tbl["kafka"].as_table()) {
            cfg.kafka_brokers = kafka->get("brokers")
                ->value_or(std::string{"localhost:9092"});
            cfg.kafka_consumer_group = kafka->get("consumer_group")
                ->value_or(std::string{"gateway"});
            cfg.kafka_response_topic = kafka->get("response_topic")
                ->value_or(std::string{"gateway.responses"});
        }

        // [redis.pubsub]
        if (auto rpub = tbl["redis"]["pubsub"].as_table()) {
            cfg.redis_pubsub_host = rpub->get("host")
                ->value_or(std::string{"localhost"});
            cfg.redis_pubsub_port = static_cast<uint16_t>(
                rpub->get("port")->value_or(int64_t{6379}));
            cfg.redis_pubsub_password = rpub->get("password")
                ->value_or(std::string{});
        }

        // [redis.auth]
        if (auto rauth = tbl["redis"]["auth"].as_table()) {
            cfg.redis_auth_host = rauth->get("host")
                ->value_or(std::string{"localhost"});
            cfg.redis_auth_port = static_cast<uint16_t>(
                rauth->get("port")->value_or(int64_t{6379}));
            cfg.redis_auth_password = rauth->get("password")
                ->value_or(std::string{});
        }

        // [timeouts]
        if (auto timeouts = tbl["timeouts"].as_table()) {
            cfg.request_timeout = std::chrono::milliseconds{
                timeouts->get("request_timeout_ms")->value_or(int64_t{5000})};
            cfg.max_pending_per_core = static_cast<size_t>(
                timeouts->get("max_pending_per_core")->value_or(int64_t{65536}));
        }

        return cfg;

    } catch (const toml::parse_error& e) {
        spdlog::error("Gateway config parse error: {}", e.what());
        return apex::core::error(apex::core::ErrorCode::ConfigParseFailed);
    }
}

} // namespace apex::gateway
