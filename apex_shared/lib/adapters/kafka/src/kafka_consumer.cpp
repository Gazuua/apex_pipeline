#include <apex/shared/adapters/kafka/kafka_consumer.hpp>

#include <spdlog/spdlog.h>

#ifndef _WIN32
#include <unistd.h>  // pipe(), read()
#endif

namespace apex::shared::adapters::kafka {

KafkaConsumer::KafkaConsumer(const KafkaConfig& config,
                             uint32_t core_id,
                             boost::asio::io_context& io_ctx)
    : config_(config), core_id_(core_id), io_ctx_(io_ctx) {}

KafkaConsumer::~KafkaConsumer() {
    stop_consuming();
    if (rk_) {
        rd_kafka_consumer_close(rk_);
#ifndef _WIN32
        if (pipe_fds_[0] != -1) { close(pipe_fds_[0]); close(pipe_fds_[1]); }
#endif
        if (rkqu_) rd_kafka_queue_destroy(rkqu_);
        rd_kafka_destroy(rk_);
        rk_ = nullptr;
    }
}

apex::core::Result<void> KafkaConsumer::init() {
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();

    rd_kafka_conf_set(conf, "bootstrap.servers", config_.brokers.c_str(),
                      errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "group.id", config_.consumer_group.c_str(),
                      errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "client.id",
                      (config_.client_id + "-consumer-" + std::to_string(core_id_)).c_str(),
                      errstr, sizeof(errstr));
    // auto.offset.reset = earliest (for new consumer groups)
    rd_kafka_conf_set(conf, "auto.offset.reset", "earliest",
                      errstr, sizeof(errstr));
    // enable.auto.commit = true (default, kept for convenience)
    rd_kafka_conf_set(conf, "enable.auto.commit", "true",
                      errstr, sizeof(errstr));

    rk_ = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk_) {
        spdlog::error("KafkaConsumer[core={}] init failed: {}", core_id_, errstr);
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Get consumer queue
    rd_kafka_poll_set_consumer(rk_);
    rkqu_ = rd_kafka_queue_get_consumer(rk_);

    // Subscribe to topics
    if (!config_.consume_topics.empty()) {
        rd_kafka_topic_partition_list_t* topics =
            rd_kafka_topic_partition_list_new(
                static_cast<int>(config_.consume_topics.size()));
        for (const auto& t : config_.consume_topics) {
            rd_kafka_topic_partition_list_add(topics, t.c_str(),
                                              RD_KAFKA_PARTITION_UA);
        }
        rd_kafka_resp_err_t err = rd_kafka_subscribe(rk_, topics);
        rd_kafka_topic_partition_list_destroy(topics);

        if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            spdlog::error("KafkaConsumer[core={}] subscribe failed: {}",
                          core_id_, rd_kafka_err2str(err));
            return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
    }

#ifndef _WIN32
    // Linux: pipe fd -> rd_kafka_queue_io_event_enable
    if (pipe(pipe_fds_) == -1) {
        spdlog::error("KafkaConsumer[core={}] pipe() failed", core_id_);
        return std::unexpected(apex::core::ErrorCode::AdapterError);
    }
    rd_kafka_queue_io_event_enable(rkqu_, pipe_fds_[1], "1", 1);
    pipe_desc_ = std::make_unique<boost::asio::posix::stream_descriptor>(
        io_ctx_, pipe_fds_[0]);
#endif

    spdlog::info("KafkaConsumer[core={}] initialized: group={}",
                  core_id_, config_.consumer_group);
    return {};
}

void KafkaConsumer::set_message_callback(MessageCallback cb) {
    message_cb_ = std::move(cb);
}

void KafkaConsumer::start_consuming() {
    consuming_ = true;
#ifndef _WIN32
    if (pipe_desc_) {
        schedule_async_wait();
    } else {
        schedule_timer_poll();
    }
#else
    schedule_timer_poll();
#endif
}

void KafkaConsumer::stop_consuming() {
    consuming_ = false;
#ifndef _WIN32
    if (pipe_desc_) pipe_desc_->cancel();
#endif
    if (poll_timer_) poll_timer_->cancel();
}

void KafkaConsumer::poll_messages() {
    if (!rk_ || !consuming_) return;

    // Non-blocking batch poll
    constexpr int max_batch = 64;
    for (int i = 0; i < max_batch; ++i) {
        rd_kafka_message_t* msg = rd_kafka_consumer_poll(rk_, 0);
        if (!msg) break;

        if (msg->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
            if (message_cb_) {
                std::span<const uint8_t> key_span;
                if (msg->key && msg->key_len > 0) {
                    key_span = {static_cast<const uint8_t*>(msg->key),
                                msg->key_len};
                }
                std::span<const uint8_t> payload_span;
                if (msg->payload && msg->len > 0) {
                    payload_span = {static_cast<const uint8_t*>(msg->payload),
                                    msg->len};
                }
                message_cb_(rd_kafka_topic_name(msg->rkt),
                             msg->partition,
                             key_span, payload_span,
                             msg->offset);
            }
            ++total_consumed_;
        } else if (msg->err != RD_KAFKA_RESP_ERR__PARTITION_EOF) {
            spdlog::warn("KafkaConsumer[core={}] poll error: {}",
                          core_id_, rd_kafka_message_errstr(msg));
        }
        rd_kafka_message_destroy(msg);
    }
}

void KafkaConsumer::schedule_async_wait() {
#ifndef _WIN32
    if (!consuming_ || !pipe_desc_) return;

    pipe_desc_->async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this](const boost::system::error_code& ec) {
            if (ec || !consuming_) return;

            // Consume event bytes from pipe
            char buf[64];
            (void)::read(pipe_fds_[0], buf, sizeof(buf));

            poll_messages();

            // Wait for next event
            schedule_async_wait();
        });
#endif
}

void KafkaConsumer::schedule_timer_poll() {
    if (!consuming_) return;

    if (!poll_timer_) {
        poll_timer_ = std::make_unique<boost::asio::steady_timer>(io_ctx_);
    }
    poll_timer_->expires_after(config_.consumer_poll_interval);
    poll_timer_->async_wait([this](const boost::system::error_code& ec) {
        if (ec || !consuming_) return;
        poll_messages();
        schedule_timer_poll();
    });
}

} // namespace apex::shared::adapters::kafka
