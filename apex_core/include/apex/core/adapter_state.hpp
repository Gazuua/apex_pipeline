// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <cstdint>

namespace apex::core
{

/// 어댑터 라이프사이클 상태.
/// CLOSED → RUNNING (init 후) → DRAINING (drain 후) → CLOSED (close 후).
enum class AdapterState : uint8_t
{
    CLOSED,  ///< 초기값 / 리소스 해제 완료
    RUNNING, ///< 초기화 완료, 요청 수락 중
    DRAINING ///< 새 요청 거부, 진행 중 요청은 허용
};

} // namespace apex::core
