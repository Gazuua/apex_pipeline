// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_registry.hpp>

#include <fmt/format.h>

#include <cassert>
#include <iterator>
#include <string>
#include <unordered_set>

namespace apex::core
{

Counter& MetricsRegistry::counter(std::string_view name, std::string_view help, Labels labels)
{
    assert(!frozen_ && "MetricsRegistry: registration after freeze()");
    auto& ptr = owned_counters_.emplace_back(std::make_unique<Counter>());
    entries_.push_back({std::string(name), std::string(help), MetricType::COUNTER, std::move(labels), ptr.get()});
    return *ptr;
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string_view help, Labels labels)
{
    assert(!frozen_ && "MetricsRegistry: registration after freeze()");
    auto& ptr = owned_gauges_.emplace_back(std::make_unique<Gauge>());
    entries_.push_back({std::string(name), std::string(help), MetricType::GAUGE, std::move(labels), ptr.get()});
    return *ptr;
}

void MetricsRegistry::counter_from(std::string_view name, std::string_view help, Labels labels,
                                   const std::atomic<uint64_t>& source)
{
    assert(!frozen_ && "MetricsRegistry: registration after freeze()");
    entries_.push_back({std::string(name), std::string(help), MetricType::COUNTER, std::move(labels), &source});
}

void MetricsRegistry::gauge_fn(std::string_view name, std::string_view help, Labels labels,
                               std::function<int64_t()> reader)
{
    assert(!frozen_ && "MetricsRegistry: registration after freeze()");
    entries_.push_back({std::string(name), std::string(help), MetricType::GAUGE, std::move(labels), std::move(reader)});
}

namespace
{

/// Escape label value per Prometheus text exposition format (RFC-like):
/// \ -> \\, " -> \", newline -> \n
void escape_label_value(fmt::memory_buffer& buf, std::string_view value)
{
    for (char c : value)
    {
        switch (c)
        {
            case '\\':
                buf.push_back('\\');
                buf.push_back('\\');
                break;
            case '"':
                buf.push_back('\\');
                buf.push_back('"');
                break;
            case '\n':
                buf.push_back('\\');
                buf.push_back('n');
                break;
            default:
                buf.push_back(c);
                break;
        }
    }
}

void format_labels(fmt::memory_buffer& buf, const Labels& labels)
{
    if (labels.empty())
        return;
    buf.push_back('{');
    for (size_t i = 0; i < labels.size(); ++i)
    {
        if (i > 0)
            buf.push_back(',');
        fmt::format_to(std::back_inserter(buf), "{}=\"", labels[i].first);
        escape_label_value(buf, labels[i].second);
        buf.push_back('"');
    }
    buf.push_back('}');
}

} // anonymous namespace

std::string MetricsRegistry::serialize() const
{
    fmt::memory_buffer buf;

    // HELP/TYPE deduplication — handles non-contiguous same-name entries safely
    std::unordered_set<std::string> emitted_names;

    for (const auto& entry : entries_)
    {
        // Emit HELP + TYPE only once per metric name
        if (emitted_names.insert(entry.name).second)
        {
            fmt::format_to(std::back_inserter(buf), "# HELP {} {}\n", entry.name, entry.help);
            fmt::format_to(std::back_inserter(buf), "# TYPE {} {}\n", entry.name,
                           entry.type == MetricType::COUNTER ? "counter" : "gauge");
        }

        // Metric name + labels
        fmt::format_to(std::back_inserter(buf), "{}", entry.name);
        format_labels(buf, entry.labels);

        // Value from source variant
        std::visit(
            [&buf](auto&& src) {
                using T = std::decay_t<decltype(src)>;
                if constexpr (std::is_same_v<T, Counter*>)
                {
                    fmt::format_to(std::back_inserter(buf), " {}\n", src->value());
                }
                else if constexpr (std::is_same_v<T, const std::atomic<uint64_t>*>)
                {
                    fmt::format_to(std::back_inserter(buf), " {}\n", src->load(std::memory_order_relaxed));
                }
                else if constexpr (std::is_same_v<T, Gauge*>)
                {
                    fmt::format_to(std::back_inserter(buf), " {}\n", src->value());
                }
                else if constexpr (std::is_same_v<T, std::function<int64_t()>>)
                {
                    // Guard against callback exceptions — one broken gauge must not poison entire scrape
                    try
                    {
                        fmt::format_to(std::back_inserter(buf), " {}\n", src());
                    }
                    catch (...)
                    {
                        fmt::format_to(std::back_inserter(buf), " 0\n");
                    }
                }
            },
            entry.source);
    }

    return fmt::to_string(buf);
}

} // namespace apex::core
