#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace apex::gateway {

struct TlsConfig {
    std::string cert_file;            // PEM certificate path
    std::string key_file;             // PEM private key path
    std::string ca_file;              // CA certificate (client verification, optional)
    bool require_client_cert = false; // mTLS (future)
};

struct JwtConfig {
    std::string secret;               // HMAC secret key (HS256)
    std::string public_key_file;      // RSA/EC public key file (RS256/ES256, future)
    std::string algorithm = "HS256";
    std::chrono::seconds clock_skew{30};     // Time tolerance
    std::vector<uint32_t> sensitive_msg_ids; // Redis blacklist check targets
};

struct RouteEntry {
    uint32_t range_begin;             // msg_id range start (inclusive)
    uint32_t range_end;               // msg_id range end (inclusive)
    std::string kafka_topic;          // Target Kafka topic
};

struct RateLimitIpConfig {
    uint32_t total_limit = 1000;          // Total requests per window (divided by num_cores)
    uint32_t window_size_seconds = 60;
    uint32_t max_entries = 65536;
    uint32_t ttl_multiplier = 2;          // TTL = window_size * ttl_multiplier
};

struct RateLimitUserConfig {
    uint32_t default_limit = 100;
    uint32_t window_size_seconds = 60;
};

struct RateLimitEndpointConfig {
    uint32_t default_limit = 60;
    uint32_t window_size_seconds = 60;
    std::vector<std::pair<uint32_t, uint32_t>> overrides;  // {msg_id, limit}
};

struct RateLimitConfig {
    RateLimitIpConfig ip;
    RateLimitUserConfig user;
    RateLimitEndpointConfig endpoint;
};

struct GatewayConfig {
    // Network
    uint16_t ws_port = 8443;          // WebSocket + TLS port
    uint32_t num_cores = 1;

    // TLS
    TlsConfig tls;

    // JWT
    JwtConfig jwt;

    // Routing
    std::vector<RouteEntry> routes;   // msg_id range -> Kafka topic mapping
    uint32_t system_range_end = 999;  // System msg_id range [0, 999]

    // Kafka
    std::string kafka_brokers = "localhost:9092";
    std::string kafka_consumer_group = "gateway";
    std::string kafka_response_topic = "gateway.responses";

    // Redis (Pub/Sub)
    std::string redis_pubsub_host = "localhost";
    uint16_t redis_pubsub_port = 6379;
    std::string redis_pubsub_password;
    std::vector<std::string> global_channels = {"pub:global:chat"};

    // Redis (Auth / blacklist)
    std::string redis_auth_host = "localhost";
    uint16_t redis_auth_port = 6379;
    std::string redis_auth_password;

    // Timeouts
    std::chrono::milliseconds request_timeout{5000};  // Pending request timeout
    size_t max_pending_per_core = 65536;               // per-core pending map max size

    // Rate Limiting
    RateLimitConfig rate_limit;
};

} // namespace apex::gateway
