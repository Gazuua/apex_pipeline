// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <libpq-fe.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace apex::shared::adapters::pg
{

/// RAII wrapper around PGresult. Automatically calls PQclear() on destruction.
class PgResult
{
  public:
    PgResult() = default;
    explicit PgResult(PGresult* result)
        : result_(result, &PQclear)
    {}

    /// Query succeeded (PGRES_COMMAND_OK or PGRES_TUPLES_OK)
    [[nodiscard]] bool ok() const noexcept;

    /// PostgreSQL error message
    [[nodiscard]] std::string_view error_message() const noexcept;

    /// Result status code
    [[nodiscard]] ExecStatusType status() const noexcept;

    /// Number of rows / columns
    [[nodiscard]] int row_count() const noexcept;
    [[nodiscard]] int column_count() const noexcept;

    /// Cell value access (text format)
    [[nodiscard]] std::string_view value(int row, int col) const noexcept;
    [[nodiscard]] bool is_null(int row, int col) const noexcept;

    /// Column name
    [[nodiscard]] std::string_view column_name(int col) const noexcept;

    /// Affected row count (INSERT/UPDATE/DELETE)
    [[nodiscard]] int affected_rows() const noexcept;

    /// Check if result is valid
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return result_ != nullptr;
    }

    /// Raw PGresult access (escape hatch)
    [[nodiscard]] PGresult* get() const noexcept
    {
        return result_.get();
    }

  private:
    std::unique_ptr<PGresult, decltype(&PQclear)> result_{nullptr, &PQclear};
};

} // namespace apex::shared::adapters::pg
