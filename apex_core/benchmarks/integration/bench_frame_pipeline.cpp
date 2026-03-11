#include "../bench_helpers.hpp"

#include <apex/core/frame_codec.hpp>
#include <apex/core/message_dispatcher.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/result.hpp>
#include <apex/core/wire_header.hpp>

#include <benchmark/benchmark.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

using namespace apex::core;

// Full pipeline: encode frame → write to RingBuffer → decode → dispatch
static void BM_FramePipeline(benchmark::State& state) {
    auto payload_size = static_cast<size_t>(state.range(0));
    boost::asio::io_context io_ctx;

    MessageDispatcher dispatcher;
    std::atomic<uint64_t> handled{0};
    dispatcher.register_handler(0x0001,
        [&handled](SessionPtr, uint16_t, std::span<const uint8_t>) -> boost::asio::awaitable<Result<void>> {
            handled.fetch_add(1, std::memory_order_relaxed);
            co_return ok();
        });

    RingBuffer rb(payload_size * 8 + WireHeader::SIZE * 8);
    auto frame_data = apex::bench::build_frame(0x0001, payload_size);

    for (auto _ : state) {
        // Write frame to ring buffer
        rb.write(frame_data);

        // Decode
        auto frame = FrameCodec::try_decode(rb);
        if (!frame) continue;

        // Dispatch (synchronous since handler is trivial)
        boost::asio::co_spawn(io_ctx,
            dispatcher.dispatch(nullptr, frame->header.msg_id, frame->payload),
            boost::asio::detached);
        io_ctx.run();
        io_ctx.restart();

        FrameCodec::consume_frame(rb, *frame);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FramePipeline)->Range(64, 4096);
