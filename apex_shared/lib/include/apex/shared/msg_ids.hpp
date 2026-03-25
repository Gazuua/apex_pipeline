// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::shared
{

/// 시스템 메시지 ID 상수 (msg_registry.toml [system] 범위).
/// Gateway가 직접 처리하는 프레임워크 레벨 메시지용이며, 서비스 라우팅을 거치지 않는다.
/// 서비스별 msg_id는 각 서비스 헤더에 정의 (auth: 1000~, chat: 2000~ 등).
struct system_msg_ids
{
    static constexpr uint32_t AUTHENTICATE_SESSION = 3;
    static constexpr uint32_t SUBSCRIBE_CHANNEL = 4;
    static constexpr uint32_t UNSUBSCRIBE_CHANNEL = 5;
};

} // namespace apex::shared
