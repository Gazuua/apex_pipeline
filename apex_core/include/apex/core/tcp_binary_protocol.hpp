#pragma once

/// Forwarding header — 실제 구현은 apex_shared/lib/protocols/tcp/로 이동됨.
/// 기존 코드 호환성을 위해 apex::core 네임스페이스에 using 선언을 유지한다.
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>

namespace apex::core
{
using apex::shared::protocols::tcp::TcpBinaryProtocol;
} // namespace apex::core
