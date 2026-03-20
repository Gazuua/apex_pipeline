// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/wire_header.hpp>

#include <benchmark/benchmark.h>
#include <boost/asio/ip/tcp.hpp>

#include <cstring>
#include <utility>
#include <vector>

namespace apex::bench
{

/// TCP loopback socket pair for benchmark fixtures.
inline std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket>
make_socket_pair(boost::asio::io_context& io)
{
    using tcp = boost::asio::ip::tcp;
    tcp::acceptor acc(io, tcp::endpoint(tcp::v4(), 0));
    auto port = acc.local_endpoint().port();

    tcp::socket client(io);
    client.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    auto server = acc.accept();
    return {std::move(server), std::move(client)};
}

/// Build a complete wire frame (12B header + payload).
inline std::vector<uint8_t> build_frame(uint32_t msg_id, size_t payload_size)
{
    std::vector<uint8_t> frame(apex::core::WireHeader::SIZE + payload_size);

    apex::core::WireHeader header;
    header.msg_id = msg_id;
    header.body_size = static_cast<uint32_t>(payload_size);
    header.serialize(std::span<uint8_t, apex::core::WireHeader::SIZE>(frame.data(), apex::core::WireHeader::SIZE));

    std::memset(frame.data() + apex::core::WireHeader::SIZE, 0xAB, payload_size);
    return frame;
}

} // namespace apex::bench
