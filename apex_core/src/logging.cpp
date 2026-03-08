#include <apex/core/logging.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace apex::core {

namespace {

spdlog::level::level_enum parse_level(const std::string& level_str) {
    // spdlog::level::from_str handles: trace, debug, info, warn, err, critical, off
    return spdlog::level::from_str(level_str);
}

/// JSON formatter for FileSink.
/// Output: {"ts":"...","level":"...","logger":"...","msg":"..."}
class JsonFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        using namespace std::chrono;
        auto time = msg.time;
        auto ms = duration_cast<milliseconds>(time.time_since_epoch()) % 1000;
        auto tt = system_clock::to_time_t(time);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        fmt::format_to(std::back_inserter(dest),
            R"({{"ts":"{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}","level":"{}","logger":"{}","msg":")"
            , tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday
            , tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count())
            , spdlog::level::to_string_view(msg.level)
            , msg.logger_name);

        // JSON-escape the message payload (RFC 8259)
        // I-17: Index-based loop to detect multi-byte UTF-8 sequences (U+2028/U+2029)
        auto msg_sv = std::string_view(msg.payload.data(), msg.payload.size());
        for (size_t i = 0; i < msg_sv.size(); ++i) {
            char c = msg_sv[i];
            switch (c) {
                case '"':  dest.push_back('\\'); dest.push_back('"'); break;
                case '\\': dest.push_back('\\'); dest.push_back('\\'); break;
                case '\b': dest.push_back('\\'); dest.push_back('b'); break;
                case '\f': dest.push_back('\\'); dest.push_back('f'); break;
                case '\n': dest.push_back('\\'); dest.push_back('n'); break;
                case '\r': dest.push_back('\\'); dest.push_back('r'); break;
                case '\t': dest.push_back('\\'); dest.push_back('t'); break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // RFC 8259: escape control characters as \u00XX
                        fmt::format_to(std::back_inserter(dest), "\\u{:04x}", static_cast<unsigned char>(c));
                    } else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < msg_sv.size()) {
                        // RFC 8259 §8.2: U+2028 (LINE SEPARATOR) and U+2029 (PARAGRAPH SEPARATOR)
                        // are valid JSON but break JavaScript eval — escape for interoperability.
                        auto c1 = static_cast<unsigned char>(msg_sv[i + 1]);
                        auto c2 = static_cast<unsigned char>(msg_sv[i + 2]);
                        if (c1 == 0x80 && (c2 == 0xA8 || c2 == 0xA9)) {
                            auto esc = std::string_view(c2 == 0xA8 ? "\\u2028" : "\\u2029");
                            dest.append(esc.data(), esc.data() + esc.size());
                            i += 2;  // skip next 2 bytes of the 3-byte UTF-8 sequence
                        } else {
                            dest.push_back(c);
                        }
                    } else {
                        dest.push_back(c);
                    }
                    break;
            }
        }
        dest.push_back('"');
        dest.push_back('}');
        dest.push_back('\n');
    }

    [[nodiscard]] std::unique_ptr<formatter> clone() const override {
        return std::make_unique<JsonFormatter>();
    }
};

} // anonymous namespace

// 재초기화 지원: shutdown_logging() 후 다시 호출해도 안전
void init_logging(const LogConfig& config) {
    // 기존 로거 정리 (재초기화 지원)
    spdlog::drop_all();

    std::vector<spdlog::sink_ptr> sinks;

    // ConsoleSink — 텍스트 포맷
    if (config.console.enabled) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        sinks.push_back(console_sink);
    }

    // FileSink — JSON 포맷 (rotating)
    if (config.file.enabled) {
        // I-16: Guard against multiplication overflow in max_size_mb * 1024 * 1024.
        // 10240 MB (10 GB) is a practical upper bound for a single rotating log file.
        if (config.file.max_size_mb > 10240) {
            throw std::invalid_argument(
                "LogConfig::file::max_size_mb must be <= 10240 (10 GB)");
        }
        auto dir = std::filesystem::path(config.file.path).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.file.path,
            config.file.max_size_mb * 1024 * 1024,
            config.file.max_files);
        if (config.file.json) {
            file_sink->set_formatter(std::make_unique<JsonFormatter>());
        }
        sinks.push_back(file_sink);
    }

    // "apex" logger (framework)
    auto apex_logger = std::make_shared<spdlog::logger>("apex", sinks.begin(), sinks.end());
    apex_logger->set_level(parse_level(config.framework_level));
    spdlog::register_logger(apex_logger);

    // "app" logger (service)
    auto app_logger = std::make_shared<spdlog::logger>("app", sinks.begin(), sinks.end());
    app_logger->set_level(parse_level(config.level));
    spdlog::register_logger(app_logger);
}

// spdlog::shutdown()은 모든 로거를 drop + thread pool 정리. 이후 init_logging() 재호출 안전.
void shutdown_logging() {
    spdlog::shutdown();
}

} // namespace apex::core
