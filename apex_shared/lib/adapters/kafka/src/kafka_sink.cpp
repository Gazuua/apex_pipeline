#include <apex/shared/adapters/kafka/kafka_sink.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace apex::shared::adapters::kafka {

KafkaSink::KafkaSink(KafkaProducer& producer, std::string topic)
    : producer_(producer), topic_(std::move(topic)) {}

void KafkaSink::sink_it_(const spdlog::details::log_msg& msg) {
    auto json = format_json(msg);
    // fire-and-forget -- errors handled in delivery callback
    (void)producer_.produce(topic_, "", std::string_view(json));
}

void KafkaSink::flush_() {
    producer_.poll(0);
}

std::string KafkaSink::format_json(const spdlog::details::log_msg& msg) const {
    // ISO 8601 timestamp
    auto time_t = std::chrono::system_clock::to_time_t(msg.time);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        msg.time.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << "{\"ts\":\"";

    // UTC time
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t);
#else
    gmtime_r(&time_t, &tm_buf);
#endif
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "Z\"";

    // level
    oss << ",\"level\":\"" << spdlog::level::to_string_view(msg.level).data() << "\"";

    // logger name
    oss << ",\"logger\":\"" << msg.logger_name.data() << "\"";

    // message (simplified JSON escape -- double quotes, backslash, newlines)
    oss << ",\"msg\":\"";
    std::string_view payload(msg.payload.data(), msg.payload.size());
    for (char c : payload) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
    oss << "\"";

    // source location (if available)
    if (!msg.source.empty()) {
        oss << ",\"file\":\"" << msg.source.filename << "\""
            << ",\"line\":" << msg.source.line;
    }

    // trace_id: extract from spdlog MDC (thread-local storage)
    // spdlog 1.14+ supports spdlog::mdc
    // Placeholder for future core trace context integration
    // auto trace_id = spdlog::mdc::get("trace_id");
    // if (!trace_id.empty()) {
    //     oss << ",\"trace_id\":\"" << trace_id << "\"";
    // }

    oss << "}";
    return oss.str();
}

} // namespace apex::shared::adapters::kafka
