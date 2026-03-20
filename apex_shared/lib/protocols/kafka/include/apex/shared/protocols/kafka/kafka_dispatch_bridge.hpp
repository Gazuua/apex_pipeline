// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/error_code.hpp>
#include <apex/core/kafka_message_meta.hpp>
#include <apex/core/result.hpp>
#include <apex/shared/protocols/kafka/kafka_envelope.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace apex::shared::protocols::kafka
{

/// Kafka 메시지를 서비스의 kafka_route 핸들러로 디스패치하는 브릿지.
///
/// Kafka consumer가 수신한 raw 바이트를 파싱하여:
/// 1. RoutingHeader에서 msg_id를 추출
/// 2. MetadataPrefix에서 메타데이터를 추출
/// 3. 등록된 핸들러 맵에서 msg_id로 핸들러를 조회
/// 4. 핸들러에 메타데이터 + payload를 전달
///
/// 사용 예시:
///   // 서비스의 on_wire()에서 브릿지 생성
///   bridge_ = KafkaDispatchBridge(kafka_handler_map());
///   // Kafka consumer 콜백에서
///   co_await bridge_.dispatch(raw_message);
class KafkaDispatchBridge
{
  public:
    /// 핸들러 함수 시그니처 — ServiceBase::KafkaHandler와 동일.
    using Handler = std::function<boost::asio::awaitable<apex::core::Result<void>>(apex::core::KafkaMessageMeta,
                                                                                   uint32_t, std::span<const uint8_t>)>;

    /// 핸들러 맵 타입 — msg_id → Handler.
    using HandlerMap = boost::unordered_flat_map<uint32_t, Handler>;

    KafkaDispatchBridge() = default;

    /// 핸들러 맵 참조로 초기화.
    /// @param handlers ServiceBase::kafka_handler_map()의 반환값
    /// @note handlers의 수명이 이 브릿지보다 길어야 한다.
    explicit KafkaDispatchBridge(const HandlerMap& handlers);

    /// Raw Kafka 메시지를 파싱하여 등록된 핸들러로 디스패치.
    ///
    /// @param raw_message [RoutingHeader][MetadataPrefix][ReplyTopic?][Payload] 형식의 raw 바이트
    /// @return 핸들러 실행 결과. 파싱 실패 시 ParseFailed, 핸들러 미등록 시 HandlerNotFound.
    [[nodiscard]] boost::asio::awaitable<apex::core::Result<void>> dispatch(std::span<const uint8_t> raw_message);

    /// 특정 msg_id에 대한 핸들러가 등록되어 있는지 확인.
    [[nodiscard]] bool has_handler(uint32_t msg_id) const noexcept;

  private:
    const HandlerMap* handlers_{nullptr};
};

} // namespace apex::shared::protocols::kafka
