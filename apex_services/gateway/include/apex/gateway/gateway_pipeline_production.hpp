// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

/// Production pipeline type alias.
/// 이 헤더를 include하면 GatewayPipeline (concrete 타입 인스턴스화)을 사용할 수 있다.
/// 테스트에서 mock 타입이 필요하면 gateway_pipeline.hpp만 include한다.

#pragma once

#include <apex/gateway/gateway_pipeline.hpp>
#include <apex/gateway/jwt_blacklist.hpp>
#include <apex/gateway/jwt_verifier.hpp>
#include <apex/shared/rate_limit/rate_limit_facade.hpp>

namespace apex::gateway
{
/// Production pipeline: zero-cost, all calls inline to concrete types.
using GatewayPipeline = GatewayPipelineBase<JwtVerifier, JwtBlacklist, apex::shared::rate_limit::RateLimitFacade>;
} // namespace apex::gateway
