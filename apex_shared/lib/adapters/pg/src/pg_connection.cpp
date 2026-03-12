#include <apex/shared/adapters/pg/pg_connection.hpp>
#include <apex/shared/adapters/pg/pg_result.hpp>

#include <spdlog/spdlog.h>

#include <boost/asio/socket_base.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>

namespace apex::shared::adapters::pg {

// =============================================================================
// PgResult implementation
// =============================================================================

bool PgResult::ok() const noexcept {
    if (!result_) return false;
    auto s = PQresultStatus(result_.get());
    return s == PGRES_COMMAND_OK || s == PGRES_TUPLES_OK;
}

ExecStatusType PgResult::status() const noexcept {
    return result_ ? PQresultStatus(result_.get()) : PGRES_FATAL_ERROR;
}

std::string_view PgResult::error_message() const noexcept {
    if (!result_) return "no result";
    const char* msg = PQresultErrorMessage(result_.get());
    return msg ? msg : "";
}

int PgResult::row_count() const noexcept {
    return result_ ? PQntuples(result_.get()) : 0;
}

int PgResult::column_count() const noexcept {
    return result_ ? PQnfields(result_.get()) : 0;
}

std::string_view PgResult::value(int row, int col) const noexcept {
    if (!result_ || PQgetisnull(result_.get(), row, col)) return {};
    return PQgetvalue(result_.get(), row, col);
}

bool PgResult::is_null(int row, int col) const noexcept {
    return !result_ || PQgetisnull(result_.get(), row, col);
}

std::string_view PgResult::column_name(int col) const noexcept {
    if (!result_) return {};
    const char* name = PQfname(result_.get(), col);
    return name ? name : "";
}

int PgResult::affected_rows() const noexcept {
    if (!result_) return 0;
    const char* val = PQcmdTuples(result_.get());
    if (!val || val[0] == '\0') return 0;
    return std::atoi(val);
}

// =============================================================================
// PgConnection implementation
// =============================================================================

PgConnection::PgConnection(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx) {}

PgConnection::~PgConnection() {
    close();
}

PgConnection::PgConnection(PgConnection&& other) noexcept
    : io_ctx_(other.io_ctx_)
    , conn_(std::exchange(other.conn_, nullptr))
    , socket_(std::move(other.socket_))
    , connected_(std::exchange(other.connected_, false)) {}

PgConnection& PgConnection::operator=(PgConnection&& other) noexcept {
    if (this != &other) {
        close();
        conn_ = std::exchange(other.conn_, nullptr);
        socket_ = std::move(other.socket_);
        connected_ = std::exchange(other.connected_, false);
    }
    return *this;
}

boost::asio::awaitable<apex::core::Result<void>>
PgConnection::connect_async(std::string_view conninfo) {
    // Ensure clean state
    if (conn_) {
        close();
    }

    // 1. Start async connection
    std::string conninfo_str(conninfo);
    conn_ = PQconnectStart(conninfo_str.c_str());
    if (!conn_) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // 2. Check initial status
    if (PQstatus(conn_) == CONNECTION_BAD) {
        spdlog::error("PgConnection: PQconnectStart failed: {}",
                      PQerrorMessage(conn_));
        PQfinish(conn_);
        conn_ = nullptr;
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // 3. Set non-blocking mode
    if (PQsetnonblocking(conn_, 1) != 0) {
        spdlog::error("PgConnection: PQsetnonblocking failed");
        PQfinish(conn_);
        conn_ = nullptr;
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // 4. Get socket fd and assign to Asio
    int fd = PQsocket(conn_);
    if (fd < 0) {
        spdlog::error("PgConnection: PQsocket returned invalid fd");
        PQfinish(conn_);
        conn_ = nullptr;
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    socket_ = std::make_unique<boost::asio::ip::tcp::socket>(io_ctx_);
    boost::system::error_code ec;
    socket_->assign(
        boost::asio::ip::tcp::v4(),
        static_cast<boost::asio::ip::tcp::socket::native_handle_type>(fd),
        ec);
    if (ec) {
        spdlog::error("PgConnection: socket assign failed: {}", ec.message());
        socket_.reset();
        PQfinish(conn_);
        conn_ = nullptr;
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // 5. PQconnectPoll loop
    co_return co_await poll_connect();
}

boost::asio::awaitable<apex::core::Result<void>>
PgConnection::poll_connect() {
    for (;;) {
        auto status = PQconnectPoll(conn_);
        switch (status) {
            case PGRES_POLLING_READING:
                co_await socket_->async_wait(
                    boost::asio::socket_base::wait_read,
                    boost::asio::use_awaitable);
                break;

            case PGRES_POLLING_WRITING:
                co_await socket_->async_wait(
                    boost::asio::socket_base::wait_write,
                    boost::asio::use_awaitable);
                break;

            case PGRES_POLLING_OK:
                connected_ = true;
                co_return apex::core::Result<void>{};

            case PGRES_POLLING_FAILED:
            default:
                spdlog::error("PgConnection: connect poll failed: {}",
                              PQerrorMessage(conn_));
                co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }
    }
}

boost::asio::awaitable<apex::core::Result<PgResult>>
PgConnection::query_async(std::string_view sql) {
    if (!connected_ || !conn_) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    // Use null-terminated string
    std::string sql_str(sql);
    if (PQsendQuery(conn_, sql_str.c_str()) == 0) {
        spdlog::error("PgConnection: PQsendQuery failed: {}",
                      PQerrorMessage(conn_));
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    co_return co_await collect_result();
}

boost::asio::awaitable<apex::core::Result<PgResult>>
PgConnection::query_params_async(std::string_view sql,
                                  std::span<const std::string> params) {
    if (!connected_ || !conn_) {
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    std::string sql_str(sql);

    // Build const char* array for PQsendQueryParams
    std::vector<const char*> param_values;
    param_values.reserve(params.size());
    for (const auto& p : params) {
        param_values.push_back(p.c_str());
    }

    if (PQsendQueryParams(conn_, sql_str.c_str(),
                           static_cast<int>(params.size()),
                           nullptr,  // paramTypes (auto-detect)
                           param_values.data(),
                           nullptr,  // paramLengths (text format)
                           nullptr,  // paramFormats (text format)
                           0         // resultFormat (text)
                           ) == 0) {
        spdlog::error("PgConnection: PQsendQueryParams failed: {}",
                      PQerrorMessage(conn_));
        co_return std::unexpected(apex::core::ErrorCode::AdapterError);
    }

    co_return co_await collect_result();
}

boost::asio::awaitable<apex::core::Result<int>>
PgConnection::execute_async(std::string_view sql) {
    auto result = co_await query_async(sql);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    co_return result->affected_rows();
}

boost::asio::awaitable<apex::core::Result<int>>
PgConnection::execute_params_async(std::string_view sql,
                                    std::span<const std::string> params) {
    auto result = co_await query_params_async(sql, params);
    if (!result.has_value()) {
        co_return std::unexpected(result.error());
    }
    co_return result->affected_rows();
}

boost::asio::awaitable<apex::core::Result<PgResult>>
PgConnection::collect_result() {
    PgResult last_result;

    for (;;) {
        // Wait for data to be available
        co_await socket_->async_wait(
            boost::asio::socket_base::wait_read,
            boost::asio::use_awaitable);

        // Consume input from the socket
        if (PQconsumeInput(conn_) == 0) {
            spdlog::error("PgConnection: PQconsumeInput failed: {}",
                          PQerrorMessage(conn_));
            co_return std::unexpected(apex::core::ErrorCode::AdapterError);
        }

        // Process all available results
        while (PQisBusy(conn_) == 0) {
            PGresult* raw = PQgetResult(conn_);
            if (raw == nullptr) {
                // No more results -- query complete
                if (!last_result) {
                    // No results at all
                    co_return std::unexpected(apex::core::ErrorCode::AdapterError);
                }
                co_return std::move(last_result);
            }

            last_result = PgResult(raw);
            if (!last_result.ok()) {
                // Drain remaining results to keep connection in sync
                while (PGresult* drain = PQgetResult(conn_)) {
                    PQclear(drain);
                }
                co_return std::unexpected(apex::core::ErrorCode::AdapterError);
            }
        }
    }
}

bool PgConnection::is_valid() const noexcept {
    return connected_ && conn_ && PQstatus(conn_) == CONNECTION_OK;
}

bool PgConnection::is_connected() const noexcept {
    return connected_;
}

void PgConnection::release_socket() noexcept {
    if (socket_) {
        // Release the fd from Asio WITHOUT closing it.
        // libpq owns the socket; PQfinish() will close it.
        boost::system::error_code ec;
        socket_->release(ec);
        socket_.reset();
    }
}

void PgConnection::close() noexcept {
    connected_ = false;
    // Order matters: release fd from Asio first, then PQfinish closes it.
    release_socket();
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

} // namespace apex::shared::adapters::pg
