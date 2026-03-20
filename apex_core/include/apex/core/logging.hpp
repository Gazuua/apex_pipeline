// Copyright (c) 2025-2026 Gazuua. All rights reserved. Licensed under the MIT License.

#pragma once

#include <apex/core/config.hpp>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <mutex>

namespace apex::core
{

/// LogConfig 기반 spdlog 로거 초기화.
/// "apex" (프레임워크) + "app" (서비스) 로거 생성.
/// main() 초반 1회 호출.
void init_logging(const LogConfig& config);

/// spdlog 정리. Server::run() 리턴 후 호출 (선택적).
void shutdown_logging();

/// 정확히 해당 레벨의 로그만 통과시키는 필터 sink.
/// spdlog set_level()은 최소 레벨 필터이므로, 레벨별 파일 분리에 이 래퍼 사용.
template <typename Mutex> class exact_level_sink : public spdlog::sinks::base_sink<Mutex>
{
  public:
    exact_level_sink(std::shared_ptr<spdlog::sinks::sink> inner, spdlog::level::level_enum target)
        : inner_(std::move(inner))
        , target_(target)
    {}

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (msg.level == target_)
        {
            inner_->log(msg);
        }
    }
    void flush_() override
    {
        inner_->flush();
    }

  private:
    std::shared_ptr<spdlog::sinks::sink> inner_;
    spdlog::level::level_enum target_;
};

} // namespace apex::core
