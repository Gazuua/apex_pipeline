#include <apex/core/logging.hpp>

#include <spdlog/async.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <filesystem>
#include <memory>
#include <regex>
#include <stdexcept>
#include <vector>

namespace apex::core
{

namespace
{

namespace fs = std::filesystem;

spdlog::level::level_enum parse_level(const std::string& level_str)
{
    return spdlog::level::from_str(level_str);
}

/// JSON formatter for FileSink.
/// Output: {"ts":"...","level":"...","logger":"...","msg":"..."}
class JsonFormatter : public spdlog::formatter
{
  public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override
    {
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
        fmt::format_to(
            std::back_inserter(dest),
            R"({{"ts":"{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}","level":"{}","logger":"{}","msg":")",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
            static_cast<int>(ms.count()), spdlog::level::to_string_view(msg.level), msg.logger_name);

        // JSON-escape the message payload (RFC 8259)
        auto msg_sv = std::string_view(msg.payload.data(), msg.payload.size());
        for (size_t i = 0; i < msg_sv.size(); ++i)
        {
            char c = msg_sv[i];
            switch (c)
            {
                case '"':
                    dest.push_back('\\');
                    dest.push_back('"');
                    break;
                case '\\':
                    dest.push_back('\\');
                    dest.push_back('\\');
                    break;
                case '\b':
                    dest.push_back('\\');
                    dest.push_back('b');
                    break;
                case '\f':
                    dest.push_back('\\');
                    dest.push_back('f');
                    break;
                case '\n':
                    dest.push_back('\\');
                    dest.push_back('n');
                    break;
                case '\r':
                    dest.push_back('\\');
                    dest.push_back('r');
                    break;
                case '\t':
                    dest.push_back('\\');
                    dest.push_back('t');
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        fmt::format_to(std::back_inserter(dest), "\\u{:04x}", static_cast<unsigned char>(c));
                    }
                    else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < msg_sv.size())
                    {
                        auto c1 = static_cast<unsigned char>(msg_sv[i + 1]);
                        auto c2 = static_cast<unsigned char>(msg_sv[i + 2]);
                        if (c1 == 0x80 && (c2 == 0xA8 || c2 == 0xA9))
                        {
                            auto esc = std::string_view(c2 == 0xA8 ? "\\u2028" : "\\u2029");
                            dest.append(esc.data(), esc.data() + esc.size());
                            i += 2;
                        }
                        else
                        {
                            dest.push_back(c);
                        }
                    }
                    else
                    {
                        dest.push_back(c);
                    }
                    break;
            }
        }
        dest.push_back('"');
        dest.push_back('}');
        dest.push_back('\n');
    }

    [[nodiscard]] std::unique_ptr<formatter> clone() const override
    {
        return std::make_unique<JsonFormatter>();
    }
};

/// 현재 작업 디렉토리에서 상위로 올라가며 .git 디렉토리를 찾아 프로젝트 루트 반환.
/// 찾지 못하면 현재 작업 디렉토리 반환.
fs::path find_project_root()
{
    auto current = fs::current_path();
    auto search = current;
    while (true)
    {
        if (fs::exists(search / ".git"))
        {
            return search;
        }
        auto parent = search.parent_path();
        if (parent == search)
            break; // 루트 도달
        search = parent;
    }
    return current; // fallback: CWD
}

/// log_root 경로 resolve.
/// 빈 값 → {project_root}/logs/
/// 값 있음 → 그대로 사용 (상대/절대)
fs::path resolve_log_path(const std::string& path)
{
    if (path.empty())
    {
        return find_project_root() / "logs";
    }
    return fs::path(path);
}

void validate_service_name(const std::string& name)
{
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos)
    {
        throw std::invalid_argument("LogConfig: service_name contains path traversal characters: '" + name + "'");
    }
    static const std::regex valid_pattern("^[a-z0-9_-]+$");
    if (!std::regex_match(name, valid_pattern))
    {
        throw std::invalid_argument("LogConfig: service_name must match [a-z0-9_-]+, got: '" + name + "'");
    }
}

/// service_name이 비어있으면 "default" 사용.
std::string resolve_service_name(const std::string& name)
{
    if (!name.empty())
        return name;
    return "default";
}

} // anonymous namespace

void init_logging(const LogConfig& config)
{
    // Step 0: 기존 로거 + thread pool 완전 정리 (재초기화 안전 보장)
    spdlog::shutdown();

    std::vector<spdlog::sink_ptr> sinks;

    // ConsoleSink — 텍스트 포맷
    if (config.console.enabled)
    {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(config.pattern);
        sinks.push_back(console_sink);
    }

    // FileSinks — 레벨별 6파일 (exact_level_sink + daily_file_format_sink)
    if (config.file.enabled)
    {
        auto svc_name = resolve_service_name(config.service_name);
        validate_service_name(svc_name);

        auto log_dir = resolve_log_path(config.file.path) / svc_name;
        fs::create_directories(log_dir);

        constexpr std::array levels = {spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
                                       spdlog::level::warn,  spdlog::level::err,   spdlog::level::critical};
        constexpr std::array<const char*, 6> level_names = {"trace", "debug", "info", "warn", "error", "critical"};

        for (size_t i = 0; i < levels.size(); ++i)
        {
            auto pattern = (log_dir / (std::string("%Y%m%d_") + level_names[i] + ".log")).string();
            auto daily = std::make_shared<spdlog::sinks::daily_file_format_sink_mt>(pattern, 0, 0, false, 0);
            if (config.file.json)
            {
                daily->set_formatter(std::make_unique<JsonFormatter>());
            }
            else
            {
                daily->set_pattern(config.pattern);
            }
            auto exact = std::make_shared<exact_level_sink<std::mutex>>(daily, levels[i]);
            sinks.push_back(exact);
        }
    }

    // Async thread pool + logger 생성
    spdlog::init_thread_pool(config.async.queue_size, 1);

    auto apex_logger = std::make_shared<spdlog::async_logger>("apex", sinks.begin(), sinks.end(), spdlog::thread_pool(),
                                                              spdlog::async_overflow_policy::overrun_oldest);
    apex_logger->set_level(parse_level(config.framework_level));
    spdlog::register_logger(apex_logger);

    auto app_logger = std::make_shared<spdlog::async_logger>("app", sinks.begin(), sinks.end(), spdlog::thread_pool(),
                                                             spdlog::async_overflow_policy::overrun_oldest);
    app_logger->set_level(parse_level(config.level));
    spdlog::register_logger(app_logger);

    // spdlog::shutdown()이 default logger를 파괴하므로 "app"을 default로 설정.
    // 이후 spdlog::info() 등 전역 호출이 "app" 로거로 라우팅됨.
    spdlog::set_default_logger(app_logger);
}

// spdlog::shutdown()은 모든 로거를 drop + thread pool 정리. 이후 init_logging() 재호출 안전.
void shutdown_logging()
{
    spdlog::shutdown();
}

} // namespace apex::core
