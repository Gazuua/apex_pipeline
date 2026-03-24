// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/core/metrics_registry.hpp>

#include <fmt/format.h>

#include <iterator>

namespace apex::core
{

Counter& MetricsRegistry::counter(std::string_view name, std::string_view help, Labels labels)
{
    auto& ptr = owned_counters_.emplace_back(std::make_unique<Counter>());
    entries_.push_back({std::string(name), std::string(help), MetricType::COUNTER, std::move(labels), ptr.get()});
    return *ptr;
}

Gauge& MetricsRegistry::gauge(std::string_view name, std::string_view help, Labels labels)
{
    auto& ptr = owned_gauges_.emplace_back(std::make_unique<Gauge>());
    entries_.push_back({std::string(name), std::string(help), MetricType::GAUGE, std::move(labels), ptr.get()});
    return *ptr;
}

void MetricsRegistry::counter_from(std::string_view name, std::string_view help, Labels labels,
                                   const std::atomic<uint64_t>& source)
{
    entries_.push_back({std::string(name), std::string(help), MetricType::COUNTER, std::move(labels), &source});
}

void MetricsRegistry::gauge_fn(std::string_view name, std::string_view help, Labels labels,
                               std::function<int64_t()> reader)
{
    entries_.push_back({std::string(name), std::string(help), MetricType::GAUGE, std::move(labels), std::move(reader)});
}

namespace
{

void format_labels(fmt::memory_buffer& buf, const Labels& labels)
{
    if (labels.empty())
        return;
    buf.push_back('{');
    for (size_t i = 0; i < labels.size(); ++i)
    {
        if (i > 0)
            buf.push_back(',');
        fmt::format_to(std::back_inserter(buf), "{}=\"{}\"", labels[i].first, labels[i].second);
    }
    buf.push_back('}');
}

} // anonymous namespace

std::string MetricsRegistry::serialize() const
{
    fmt::memory_buffer buf;

    // Group entries by name for HELP/TYPE deduplication.
    // Entries with same name but different labels share one HELP/TYPE header.
    std::string_view prev_name;

    for (const auto& entry : entries_)
    {
        // Emit HELP + TYPE only once per metric name
        if (entry.name != prev_name)
        {
            fmt::format_to(std::back_inserter(buf), "# HELP {} {}\n", entry.name, entry.help);
            fmt::format_to(std::back_inserter(buf), "# TYPE {} {}\n", entry.name,
                           entry.type == MetricType::COUNTER ? "counter" : "gauge");
            prev_name = entry.name;
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
                    fmt::format_to(std::back_inserter(buf), " {}\n", src());
                }
            },
            entry.source);
    }

    return fmt::to_string(buf);
}

} // namespace apex::core
