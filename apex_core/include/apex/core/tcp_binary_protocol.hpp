// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

/// @deprecated 이 포워딩 헤더는 폐기 예정. 직접
/// #include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp> 를 사용하라.
/// apex_core → apex_shared 역방향 의존 제거 (#116).
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

namespace apex::core
{
using apex::shared::protocols::tcp::TcpBinaryProtocol;
} // namespace apex::core
