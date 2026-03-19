#include <apex/core/frame_codec.hpp>
#include <apex/core/ring_buffer.hpp>
#include <apex/core/wire_header.hpp>
#include <benchmark/benchmark.h>
#include <cstring>
#include <vector>

using namespace apex::core;

static void BM_FrameCodec_Encode(benchmark::State& state)
{
    auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer rb(payload_size * 4 + WireHeader::SIZE * 4);
    std::vector<uint8_t> payload(payload_size, 0xAB);
    WireHeader header{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(payload_size), .reserved = {}};
    for (auto _ : state)
    {
        bool ok = FrameCodec::encode(rb, header, payload);
        benchmark::DoNotOptimize(ok);
        rb.consume(WireHeader::SIZE + payload_size);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FrameCodec_Encode)->Range(64, 16384);

static void BM_FrameCodec_Decode(benchmark::State& state)
{
    auto payload_size = static_cast<size_t>(state.range(0));
    RingBuffer rb(payload_size * 4 + WireHeader::SIZE * 4);
    std::vector<uint8_t> payload(payload_size, 0xAB);
    WireHeader header{.msg_id = 0x0001, .body_size = static_cast<uint32_t>(payload_size), .reserved = {}};
    for (auto _ : state)
    {
        (void)FrameCodec::encode(rb, header, payload);
        auto frame = FrameCodec::try_decode(rb);
        benchmark::DoNotOptimize(frame);
        if (frame)
            FrameCodec::consume_frame(rb, *frame);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(WireHeader::SIZE + payload_size));
}
BENCHMARK(BM_FrameCodec_Decode)->Range(64, 16384);
