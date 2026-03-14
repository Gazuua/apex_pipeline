#include <apex/shared/adapters/pg/pg_transaction.hpp>

namespace apex::shared::adapters::pg {

PgTransaction::PgTransaction(PgConnection& conn, PgPool& pool)
    : conn_(conn), pool_(pool) {}

PgTransaction::~PgTransaction() {
    if (!finished_) {
        conn_.mark_poisoned();
    }
}

boost::asio::awaitable<apex::core::Result<PgResult>>
PgTransaction::execute(std::string_view sql) {
    co_return co_await conn_.query_async(sql);
}

boost::asio::awaitable<apex::core::Result<PgResult>>
PgTransaction::execute_params(std::string_view sql,
                               std::span<const std::string> params) {
    co_return co_await conn_.query_params_async(sql, params);
}

boost::asio::awaitable<apex::core::Result<void>>
PgTransaction::commit() {
    auto result = co_await conn_.query_async("COMMIT");
    if (result) {
        finished_ = true;
    }
    co_return result.transform([](auto&&) {});
}

boost::asio::awaitable<apex::core::Result<void>>
PgTransaction::rollback() {
    auto result = co_await conn_.query_async("ROLLBACK");
    finished_ = true;  // rollback completes regardless of success/failure
    co_return result.transform([](auto&&) {});
}

} // namespace apex::shared::adapters::pg
