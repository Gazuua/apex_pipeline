// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

// Explicit template instantiation for the production pipeline type.
// This ensures the template is compiled exactly once for the production types,
// avoiding duplicate code generation across translation units.

#include <apex/gateway/gateway_pipeline_production.hpp>

namespace apex::gateway
{

template class GatewayPipelineBase<JwtVerifier, JwtBlacklist, apex::shared::rate_limit::RateLimitFacade>;

} // namespace apex::gateway
