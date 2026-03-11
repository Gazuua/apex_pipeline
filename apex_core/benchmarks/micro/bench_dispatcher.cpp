#include <apex/core/message_dispatcher.hpp>
#include <apex/core/result.hpp>
#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

using namespace apex::core;

static void BM_Dispatcher_Lookup(benchmark::State& state) {
    auto num_handlers = static_cast<int>(state.range(0));
    boost::asio::io_context io_ctx;
    MessageDispatcher dispatcher;
    for (int i = 0; i < num_handlers; ++i) {
        dispatcher.register_handler(static_cast<uint16_t>(i),
            [](SessionPtr, uint16_t, std::span<const uint8_t>) -> boost::asio::awaitable<Result<void>> {
                co_return ok();
            });
    }
    uint16_t target_id = static_cast<uint16_t>(num_handlers / 2);
    for (auto _ : state) {
        bool has = dispatcher.has_handler(target_id);
        benchmark::DoNotOptimize(has);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Dispatcher_Lookup)->Arg(10)->Arg(100)->Arg(1000);
