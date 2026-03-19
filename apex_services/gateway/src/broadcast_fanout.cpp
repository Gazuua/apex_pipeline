#include <apex/gateway/broadcast_fanout.hpp>

#include <apex/core/cross_core_call.hpp>
#include <spdlog/spdlog.h>

#include <cstring>

namespace apex::gateway
{

BroadcastFanout::BroadcastFanout(apex::core::CoreEngine& engine, uint32_t num_cores,
                                 std::vector<apex::core::SessionManager*> session_mgrs)
    : engine_(engine)
    , num_cores_(num_cores)
    , session_mgrs_(std::move(session_mgrs))
{}

void BroadcastFanout::set_channel_maps(std::vector<ChannelSessionMap>* maps)
{
    channel_maps_ = maps;
}

void BroadcastFanout::fanout(std::string_view channel, std::span<const uint8_t> message)
{
    // Build WireHeader v2 framed data from Redis Pub/Sub message
    auto frame = build_wire_frame(message);
    if (frame.empty())
    {
        spdlog::warn("BroadcastFanout: failed to build wire frame for channel '{}'", channel);
        return;
    }

    auto data = std::make_shared<std::vector<uint8_t>>(std::move(frame));

    // Global channels (pub:global:*) — broadcast to ALL sessions on ALL cores.
    // Gateway is channel-name-agnostic except for this prefix convention.
    static constexpr std::string_view GLOBAL_PREFIX = "pub:global:";
    if (channel.substr(0, GLOBAL_PREFIX.size()) == GLOBAL_PREFIX)
    {
        for (uint32_t core_id = 0; core_id < session_mgrs_.size(); ++core_id)
        {
            auto* mgr = session_mgrs_[core_id];
            (void)apex::core::cross_core_post(engine_, core_id, [mgr, shared_data = data]() {
                mgr->for_each([&shared_data](apex::core::SessionPtr session) {
                    if (session && session->is_open())
                    {
                        (void)session->enqueue_write_raw(*shared_data);
                    }
                });
            });
        }
        return;
    }

    // Room-specific channels — 모든 코어에 post, 각 코어가 로컬 맵에서 확인.
    // per-core 맵이므로 뮤텍스 불필요 — 각 코어 스레드에서 자기 맵만 접근.
    for (uint32_t core_id = 0; core_id < num_cores_; ++core_id)
    {
        if (core_id >= session_mgrs_.size())
            continue;

        auto* mgr = session_mgrs_[core_id];
        auto* maps = channel_maps_;

        (void)apex::core::cross_core_post(engine_, core_id,
                                          [mgr, maps, core_id, ch = std::string(channel), shared_data = data]() {
                                              if (!maps)
                                                  return;
                                              auto* subs = (*maps)[core_id].get_subscribers(ch);
                                              if (!subs || subs->empty())
                                                  return;

                                              for (auto sid : *subs)
                                              {
                                                  auto session = mgr->find_session(sid);
                                                  if (!session || !session->is_open())
                                                      continue;
                                                  (void)session->enqueue_write_raw(*shared_data);
                                              }
                                          });
    }
}

std::vector<uint8_t> BroadcastFanout::build_wire_frame(std::span<const uint8_t> message)
{
    // Redis Pub/Sub message format: [4B msg_id BE] + [payload]
    // If message is shorter than 4 bytes, treat as system broadcast (msg_id=0, no payload).
    uint32_t msg_id = 0;
    std::span<const uint8_t> payload;

    if (message.size() >= PUBSUB_MSG_ID_SIZE)
    {
        // Extract big-endian msg_id
        msg_id = (static_cast<uint32_t>(message[0]) << 24) | (static_cast<uint32_t>(message[1]) << 16) |
                 (static_cast<uint32_t>(message[2]) << 8) | (static_cast<uint32_t>(message[3]));
        payload = message.subspan(PUBSUB_MSG_ID_SIZE);
    }
    else if (!message.empty())
    {
        // Malformed — too short for msg_id prefix
        spdlog::warn("BroadcastFanout: message too short ({} bytes) for msg_id prefix", message.size());
        return {};
    }
    // else: empty message -> msg_id=0, empty payload (e.g., heartbeat/signal)

    // Build WireHeader v2
    apex::core::WireHeader wh;
    wh.msg_id = msg_id;
    wh.body_size = static_cast<uint32_t>(payload.size());
    wh.flags = 0;

    // Assemble [12B WireHeader] + [payload]
    std::vector<uint8_t> frame(apex::core::WireHeader::SIZE + payload.size());
    auto hdr_buf = wh.serialize();
    std::memcpy(frame.data(), hdr_buf.data(), hdr_buf.size());

    if (!payload.empty())
    {
        std::memcpy(frame.data() + apex::core::WireHeader::SIZE, payload.data(), payload.size());
    }

    return frame;
}

} // namespace apex::gateway
