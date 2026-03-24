// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/kafka/kafka_consumer.hpp>
#include <apex/shared/adapters/kafka/kafka_producer.hpp>

#include <cstring>
#include <string>

#ifndef _WIN32
#include <unistd.h> // pipe(), read()
#endif

namespace apex::shared::adapters::kafka
{

KafkaConsumer::KafkaConsumer(const KafkaConfig& config, uint32_t core_id, boost::asio::io_context& io_ctx,
                             KafkaProducer* producer)
    : logger_("KafkaConsumer", core_id, "app")
    , config_(config)
    , core_id_(core_id)
    , io_ctx_(io_ctx)
    , producer_(producer)
{}

KafkaConsumer::~KafkaConsumer()
{
    stop_consuming();
    if (rk_)
    {
        rd_kafka_consumer_close(rk_);
#ifndef _WIN32
        // pipe_desc_ owns pipe_fds_[0] via assign() -- reset it first
        // to close pipe_fds_[0] exactly once (stream_descriptor dtor).
        pipe_desc_.reset();
        // pipe_fds_[1] is NOT owned by stream_descriptor, close manually.
        if (pipe_fds_[1] != -1)
        {
            ::close(pipe_fds_[1]);
            pipe_fds_[1] = -1;
        }
        // pipe_fds_[0] already closed by stream_descriptor, just clear.
        pipe_fds_[0] = -1;
#endif
        if (rkqu_)
            rd_kafka_queue_destroy(rkqu_);
        rd_kafka_destroy(rk_);
        rk_ = nullptr;
    }
}

apex::core::Result<void> KafkaConsumer::init()
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

    conf_set("bootstrap.servers", config_.brokers.c_str());
    conf_set("group.id", config_.consumer_group.c_str());
    conf_set("client.id", (config_.client_id + "-consumer-" + std::to_string(core_id_)).c_str());
    // auto.offset.reset = earliest (for new consumer groups)
    conf_set("auto.offset.reset", "earliest");
    // enable.auto.commit = true (default, kept for convenience)
    conf_set("enable.auto.commit", "true");

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

    rk_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk_)
    {
        logger_.error("init failed: {}", errstr);
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Get consumer queue
    rd_kafka_poll_set_consumer(rk_);
    rkqu_ = rd_kafka_queue_get_consumer(rk_);

    // Subscribe to topics
    if (!config_.consume_topics.empty())
    {
        rd_kafka_topic_partition_list_t* topics =
            rd_kafka_topic_partition_list_new(static_cast<int>(config_.consume_topics.size()));
        for (const auto& t : config_.consume_topics)
        {
            rd_kafka_topic_partition_list_add(topics, t.c_str(), RD_KAFKA_PARTITION_UA);
        }
        rd_kafka_resp_err_t err = rd_kafka_subscribe(rk_, topics);
        rd_kafka_topic_partition_list_destroy(topics);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            logger_.error("subscribe failed: {}", rd_kafka_err2str(err));
            return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
    }

#ifndef _WIN32
    // Linux: pipe fd -> rd_kafka_queue_io_event_enable
    if (pipe(pipe_fds_) == -1)
    {
        logger_.error("pipe() failed");
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    rd_kafka_queue_io_event_enable(rkqu_, pipe_fds_[1], "1", 1);
    pipe_desc_ = std::make_unique<boost::asio::posix::stream_descriptor>(io_ctx_, pipe_fds_[0]);
#endif

    logger_.info("initialized: group={}", config_.consumer_group);
    return {};
}

void KafkaConsumer::set_message_callback(MessageCallback cb)
{
    message_cb_ = std::move(cb);
}

void KafkaConsumer::start_consuming()
{
    consuming_ = true;
#ifndef _WIN32
    if (pipe_desc_)
    {
        schedule_async_wait();
    }
    else
    {
        schedule_timer_poll();
    }
#else
    schedule_timer_poll();
#endif
}

void KafkaConsumer::stop_consuming()
{
    consuming_ = false;
#ifndef _WIN32
    if (pipe_desc_)
        pipe_desc_->cancel();
#endif
    if (poll_timer_)
        poll_timer_->cancel();
}

void KafkaConsumer::poll_messages()
{
    if (!rk_ || !consuming_)
        return;

    // Non-blocking batch poll
    const int max_batch = config_.consumer_max_batch;
    int consumed_count = 0;
    for (int i = 0; i < max_batch; ++i)
    {
        rd_kafka_message_t* msg = rd_kafka_consumer_poll(rk_, 0);
        if (!msg)
            break;

        if (msg->err == RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            if (message_cb_)
            {
                std::span<const uint8_t> key_span;
                if (msg->key && msg->key_len > 0)
                {
                    key_span = {static_cast<const uint8_t*>(msg->key), msg->key_len};
                }
                std::span<const uint8_t> payload_span;
                if (msg->payload && msg->len > 0)
                {
                    payload_span = {static_cast<const uint8_t*>(msg->payload), msg->len};
                }
                auto result =
                    message_cb_(rd_kafka_topic_name(msg->rkt), msg->partition, key_span, payload_span, msg->offset);
                // DLQ routing: failed callback + producer available
                if (!result.has_value() && !producer_)
                {
                    logger_.warn("Message processing failed (topic={}, offset={}) "
                                 "but no DLQ producer configured — message dropped",
                                 rd_kafka_topic_name(msg->rkt), msg->offset);
                }
                if (!result.has_value() && producer_)
                {
                    auto dlq_topic = std::string(rd_kafka_topic_name(msg->rkt)) + "-dlq";
                    logger_.warn("Message processing failed (topic={}, offset={}), "
                                 "routing to DLQ: {}",
                                 rd_kafka_topic_name(msg->rkt), msg->offset,
                                 apex::core::error_code_name(result.error()));
                    // Convert key_span to string_view for producer API
                    std::string_view key_sv;
                    if (!key_span.empty())
                    {
                        key_sv = std::string_view(reinterpret_cast<const char*>(key_span.data()), key_span.size());
                    }
                    metric_dlq_total_.fetch_add(1, std::memory_order_relaxed);
                    auto dlq_result = producer_->produce(dlq_topic, key_sv, payload_span);
                    if (!dlq_result.has_value())
                    {
                        logger_.error("DLQ produce failed (topic={}, offset={}): "
                                      "message permanently lost",
                                      dlq_topic, msg->offset);
                    }
                }
            }
            metric_consume_total_.fetch_add(1, std::memory_order_relaxed);
            ++consumed_count;
        }
        else if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF)
        {
            logger_.warn("poll error: {}", rd_kafka_message_errstr(msg));
        }
        rd_kafka_message_destroy(msg);
    }

    if (consumed_count > 0)
    {
        logger_.trace("poll_messages: batch_size={}, consumed={}", max_batch, consumed_count);
    }
}

void KafkaConsumer::schedule_async_wait()
{
#ifndef _WIN32
    if (!consuming_ || !pipe_desc_)
        return;

    std::weak_ptr<KafkaConsumer> weak_self = shared_from_this();
    pipe_desc_->async_wait(boost::asio::posix::stream_descriptor::wait_read,
                           [weak_self](const boost::system::error_code& ec) {
                               auto self = weak_self.lock();
                               if (!self || ec || !self->consuming_)
                                   return;

                               // Consume event bytes from pipe
                               char buf[64];
                               (void)::read(self->pipe_fds_[0], buf, sizeof(buf));

                               self->poll_messages();

                               // Wait for next event
                               self->schedule_async_wait();
                           });
#endif
}

void KafkaConsumer::schedule_timer_poll()
{
    if (!consuming_)
        return;

    if (!poll_timer_)
    {
        poll_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx_);
    }
    poll_timer_->expires_after(config_.consumer_poll_interval);
    std::weak_ptr<KafkaConsumer> weak_self = shared_from_this();
    poll_timer_->async_wait([weak_self](const boost::system::error_code& ec) {
        auto self = weak_self.lock();
        if (!self || ec || !self->consuming_)
            return;
        self->poll_messages();
        self->schedule_timer_poll();
    });
}

} // namespace apex::shared::adapters::kafka
