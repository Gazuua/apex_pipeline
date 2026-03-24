// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <cstring>
#include <mutex>

namespace apex::shared::adapters::kafka
{

KafkaProducer::KafkaProducer(const KafkaConfig& config)
    : config_(config)
{}

KafkaProducer::~KafkaProducer()
{
    if (rk_)
    {
        // Destroy topic handles
        for (auto& entry : topic_cache_)
        {
            if (entry.rkt)
                rd_kafka_topic_destroy(entry.rkt);
        }
        topic_cache_.clear();

        // Flush then destroy handle
        rd_kafka_flush(rk_, static_cast<int>(config_.flush_timeout_ms));
        rd_kafka_destroy(rk_);
        rk_ = nullptr;
    }
}

apex::core::Result<void> KafkaProducer::init()
{
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

    // Helper: set config, returns false on failure.
    // Sensitive keys (sasl.password) have their values masked in logs.
    auto conf_set = [&](const char* key, const char* val) -> bool {
        if (rd_kafka_conf_set(conf, key, val, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK)
        {
            const bool sensitive = std::strcmp(key, "sasl.password") == 0;
            logger_.warn("rd_kafka_conf_set({}, {}) failed: {}", key, sensitive ? "****" : val, errstr);
            return false;
        }
        return true;
    };

    // Broker
    conf_set("bootstrap.servers", config_.brokers.c_str());

    // client.id
    conf_set("client.id", config_.client_id.c_str());

    // Producer batch settings
    conf_set("linger.ms", std::to_string(config_.producer_batch_ms.count()).c_str());
    conf_set("batch.size", std::to_string(config_.producer_batch_size).c_str());
    conf_set("compression.type", config_.compression_type.c_str());
    conf_set("queue.buffering.max.messages", std::to_string(config_.queue_buffering_max_messages).c_str());

    // Security settings (only apply non-empty values).
    // Security config failures are fatal — falling through to plaintext is worse than failing init.
    const auto& sec = config_.security;
    auto sec_set = [&](const char* key, const char* val) -> bool {
        if (!conf_set(key, val))
        {
            logger_.error("security config '{}' failed — aborting init", key);
            rd_kafka_conf_destroy(conf);
            return false;
        }
        return true;
    };
    if (sec.protocol != "PLAINTEXT")
        if (!sec_set("security.protocol", sec.protocol.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.ssl_ca_location.empty())
        if (!sec_set("ssl.ca.location", sec.ssl_ca_location.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.ssl_cert_location.empty())
        if (!sec_set("ssl.certificate.location", sec.ssl_cert_location.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.ssl_key_location.empty())
        if (!sec_set("ssl.key.location", sec.ssl_key_location.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.sasl_mechanism.empty())
        if (!sec_set("sasl.mechanism", sec.sasl_mechanism.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.sasl_username.empty())
        if (!sec_set("sasl.username", sec.sasl_username.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);
    if (!sec.sasl_password.empty())
        if (!sec_set("sasl.password", sec.sasl_password.c_str()))
            return std::unexpected(apex::core::ErrorCode::AdapterError);

    // Extra properties (pass-through)
    for (const auto& [key, val] : config_.extra_properties)
    {
        conf_set(key.c_str(), val.c_str());
    }

    // Delivery report callback
    rd_kafka_conf_set_dr_msg_cb(conf, &KafkaProducer::delivery_report_cb);
    rd_kafka_conf_set_opaque(conf, this);

    // Create producer
    // Note: on rd_kafka_new failure, conf ownership is NOT transferred --
    // but librdkafka docs say conf is always consumed by rd_kafka_new.
    rk_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk_)
    {
        logger_.error("init failed: {}", errstr);
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    logger_.info("initialized: brokers={}", config_.brokers);
    return {};
}

apex::core::Result<void> KafkaProducer::produce(std::string_view topic, std::string_view key,
                                                std::span<const uint8_t> payload)
{
    logger_.trace("produce: topic={}, partition=UA, payload_size={}", topic, payload.size());

    if (!rk_)
    {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    auto* rkt = get_or_create_topic(topic);
    if (!rkt)
    {
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    int err = rd_kafka_produce(rkt,
                               RD_KAFKA_PARTITION_UA, // auto partitioning
                               RD_KAFKA_MSG_F_COPY,   // copy payload
                               const_cast<void*>(static_cast<const void*>(payload.data())), payload.size(),
                               key.empty() ? nullptr : key.data(), key.size(),
                               nullptr // opaque (per-message)
    );

    if (err == -1)
    {
        rd_kafka_resp_err_t last_err = rd_kafka_last_error();
        logger_.warn("produce failed: topic={}, err={}", topic, rd_kafka_err2str(last_err));
        metric_produce_errors_.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    return {};
}

apex::core::Result<void> KafkaProducer::produce(std::string_view topic, std::string_view key, std::string_view payload)
{
    return produce(topic, key,
                   std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
}

int KafkaProducer::poll(int timeout_ms)
{
    if (!rk_)
        return 0;
    return rd_kafka_poll(rk_, timeout_ms);
}

bool KafkaProducer::flush(std::chrono::milliseconds timeout)
{
    if (!rk_)
        return true;
    rd_kafka_resp_err_t err = rd_kafka_flush(rk_, static_cast<int>(timeout.count()));
    return err == RD_KAFKA_RESP_ERR_NO_ERROR;
}

int32_t KafkaProducer::outq_len() const noexcept
{
    if (!rk_)
        return 0;
    return rd_kafka_outq_len(rk_);
}

void KafkaProducer::delivery_report_cb(rd_kafka_t* /*rk*/, const rd_kafka_message_t* msg, void* opaque)
{
    auto* self = static_cast<KafkaProducer*>(opaque);
    if (msg->err)
    {
        self->logger_.warn("delivery failed: topic={}, err={}", rd_kafka_topic_name(msg->rkt),
                           rd_kafka_err2str(msg->err));
        self->metric_produce_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        self->metric_produce_total_.fetch_add(1, std::memory_order_relaxed);
    }
}

rd_kafka_topic_t* KafkaProducer::get_or_create_topic(std::string_view topic)
{
    std::lock_guard lock(topic_mutex_);
    // Cache lookup
    for (auto& entry : topic_cache_)
    {
        if (entry.name == topic)
            return entry.rkt;
    }
    // Create new topic handle
    rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk_, std::string(topic).c_str(), nullptr);
    if (rkt)
    {
        topic_cache_.push_back({std::string(topic), rkt});
    }
    return rkt;
}

} // namespace apex::shared::adapters::kafka
