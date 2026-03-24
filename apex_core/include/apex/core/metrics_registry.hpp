// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace apex::core
{

/// Monotonically increasing counter for Prometheus metrics.
/// Thread-safe: single writer (owning core/thread), multiple readers (scrape thread).
class Counter
{
  public:
    void increment(uint64_t delta = 1) noexcept
    {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t value() const noexcept
    {
        return value_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<uint64_t> value_{0};
};

/// Bidirectional gauge for Prometheus metrics.
/// Thread-safe: single writer, multiple readers.
class Gauge
{
  public:
    void increment(int64_t delta = 1) noexcept
    {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }

    void decrement(int64_t delta = 1) noexcept
    {
        value_.fetch_sub(delta, std::memory_order_relaxed);
    }

    void set(int64_t v) noexcept
    {
        value_.store(v, std::memory_order_relaxed);
    }

    [[nodiscard]] int64_t value() const noexcept
    {
        return value_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<int64_t> value_{0};
};

/// Label pair for Prometheus metrics (e.g., core="0").
using Label = std::pair<std::string, std::string>;
using Labels = std::vector<Label>;

/// Central registry for Prometheus metrics.
/// Registration is single-threaded (init phase only). Value reads are lock-free (scrape thread).
///
/// Three registration patterns:
///   1. Owned: registry creates and owns Counter/Gauge, caller stores reference
///   2. Ref:   registry reads an external std::atomic (per-core component counters)
///   3. Fn:    registry calls a function at scrape time (dynamic values like active sessions)
class MetricsRegistry
{
  public:
    /// Owned pattern — registry creates Counter, returns reference for caller to increment.
    Counter& counter(std::string_view name, std::string_view help, Labels labels = {});

    /// Owned pattern — registry creates Gauge, returns reference for caller to use.
    Gauge& gauge(std::string_view name, std::string_view help, Labels labels = {});

    /// Ref pattern — register an external atomic<uint64_t> as a counter (read-only at scrape).
    void counter_from(std::string_view name, std::string_view help, Labels labels,
                      const std::atomic<uint64_t>& source);

    /// Fn pattern — register a callback that returns a gauge value at scrape time.
    void gauge_fn(std::string_view name, std::string_view help, Labels labels,
                  std::function<int64_t()> reader);

    /// Serialize all registered metrics to Prometheus text exposition format.
    [[nodiscard]] std::string serialize() const;

  private:
    enum class MetricType
    {
        COUNTER,
        GAUGE
    };

    /// Value source: owned Counter/Gauge, external atomic ref, or callback function.
    using ValueSource = std::variant<Counter*, const std::atomic<uint64_t>*, Gauge*, std::function<int64_t()>>;

    struct MetricEntry
    {
        std::string name;
        std::string help;
        MetricType type;
        Labels labels;
        ValueSource source;
    };

    std::vector<MetricEntry> entries_;

    /// Owned Counter/Gauge storage. Stable pointers via unique_ptr.
    std::vector<std::unique_ptr<Counter>> owned_counters_;
    std::vector<std::unique_ptr<Gauge>> owned_gauges_;
};

} // namespace apex::core
