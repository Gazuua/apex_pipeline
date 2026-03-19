# apex_core 프레임워크 가이드

**버전**: v0.5.5.2 | **최종 갱신**: 2026-03-19
**목적**: 이 문서 하나만 읽고 apex_core 위에 새 서비스를 올릴 수 있다.

> **설계 결정 D1-D7**: 이 문서에는 현재 코드에 아직 구현되지 않은 "의도된 설계"가 포함되어 있다.
> 해당 항목에는 `[D*]` 태그가 붙어 있으며, 코드 구현은 #48 에이전트가 담당한다.
> 구현 전까지는 현재 코드와 가이드 사이에 차이가 있을 수 있다.

---

## 목차

- [§1. 퀵 레퍼런스](#1-퀵-레퍼런스)
- [§2. Server 설정 & 부트스트랩](#2-server-설정--부트스트랩)
- [§3. 라이프사이클 훅](#3-라이프사이클-훅)
- [§4. 핸들러 & 메시지](#4-핸들러--메시지)
- [§5. 어댑터 접근](#5-어댑터-접근)
- [§6. 메모리 관리](#6-메모리-관리)
- [§7. 유틸리티](#7-유틸리티)
- [§8. 금지사항 & 안티패턴](#8-금지사항--안티패턴)
- [§9. 빌드 시스템 통합](#9-빌드-시스템-통합)
- [§10. 내부 아키텍처](#10-내부-아키텍처)
- [§11. 실전 서비스 패턴](#11-실전-서비스-패턴)

---

## §1. 퀵 레퍼런스

최소 동작 서비스의 전체 코드. 이후 섹션은 이 스켈레톤의 각 부분을 상세히 설명한다.

### TCP 서비스 스켈레톤

```cpp
// ── my_service.hpp ──────────────────────────────────────────────
#pragma once
#include <apex/core/service_base.hpp>

namespace my_app {

class MyService : public apex::core::ServiceBase<MyService> {
public:
    MyService() : ServiceBase("my_svc") {}

    // Phase 1: 어댑터 획득
    void on_configure(apex::core::ConfigureContext& ctx) override {
        // redis_ = &ctx.server.adapter<RedisAdapter>();  // §5 참조
    }

    // Phase 2: 서비스 간 와이어링
    void on_wire(apex::core::WireContext& ctx) override {
        // auto* other = ctx.local_registry.get<OtherService>();  // [D1] §3 참조
    }

    // Phase 3: 핸들러 등록
    void on_start() override {
        route<MyRequest>(0x1000, &MyService::on_request);  // §4 참조
    }

    void on_stop() override {
        // 정리 로직
    }

    void on_session_closed(apex::core::SessionId id) override {
        // per-session 상태 정리
    }

    // ── 핸들러 ──
    boost::asio::awaitable<apex::core::Result<void>>
    on_request(apex::core::SessionPtr session, uint32_t msg_id,
               const MyRequest* req) {
        // ⚠️ req 포인터는 co_await 전까지만 유효 (§8 #1)
        auto data = req->name()->str();  // co_await 전에 복사

        // 응답 빌드 + 전송
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp = CreateMyResponse(fbb, fbb.CreateString("ok"));
        fbb.Finish(resp);
        session->enqueue_write(
            apex::core::build_frame(msg_id + 1, fbb));  // §4.3 참조

        co_return apex::core::ok();
    }
};

} // namespace my_app

// ── main.cpp ────────────────────────────────────────────────────
#include "my_service.hpp"
#include <apex/core/config.hpp>
#include <apex/core/logging.hpp>
#include <apex/core/server.hpp>
#include <apex/shared/protocols/tcp/tcp_binary_protocol.hpp>
// 어댑터 사용 시: #include <apex/shared/adapters/adapter_base.hpp>
// 어댑터 사용 시: #include <apex/shared/adapters/redis/redis_adapter.hpp> 등

#include <toml++/toml.hpp>

int main(int argc, char* argv[]) {
    // 1. 설정 로드 + 로깅 초기화
    std::string config_path = (argc > 1) ? argv[1] : "my_svc.toml";
    auto app_config = apex::core::AppConfig::from_file(config_path);
    apex::core::init_logging(app_config.logging);  // §2.3 참조

    // 2. Server 구성
    apex::core::Server server({.num_cores = 1});

    server
        .listen<apex::shared::protocols::tcp::TcpBinaryProtocol>(
            9000,
            apex::shared::protocols::tcp::TcpBinaryProtocol::Config{
                .max_frame_size = 64 * 1024
            })
        // .add_adapter<RedisAdapter>(redis_config)  // §5 참조
        .add_service<my_app::MyService>();

    // 3. 실행 (블로킹, SIGINT/SIGTERM으로 graceful shutdown)
    server.run();

    apex::core::shutdown_logging();
    return 0;
}
```

### Kafka-only 서비스 스켈레톤

TCP 리스너 없이 Kafka 메시지만 처리하는 서비스. `[D2]` Kafka 자동 배선에 의해 `kafka_route<T>()` 등록만 하면 KafkaDispatchBridge가 자동 생성된다.

```cpp
class MyKafkaService : public apex::core::ServiceBase<MyKafkaService> {
public:
    MyKafkaService(Config cfg) : ServiceBase("my_kafka_svc"), config_(std::move(cfg)) {}

    void on_configure(apex::core::ConfigureContext& ctx) override {
        kafka_ = &ctx.server.adapter<KafkaAdapter>();
    }

    void on_start() override {
        // kafka_route만 등록하면 코어가 자동으로 KafkaDispatchBridge 배선 [D2]
        kafka_route<MyEvent>(0x2000, &MyKafkaService::on_event);
    }

    boost::asio::awaitable<apex::core::Result<void>>
    on_event(const apex::shared::protocols::kafka::MetadataPrefix& meta,
             uint32_t msg_id, const MyEvent* evt) {
        // meta.corr_id, meta.session_id 등 사용 가능
        co_return apex::core::ok();
    }

private:
    KafkaAdapter* kafka_{nullptr};
    Config config_;
};

// main.cpp — TCP 스켈레톤과 동일하되 listen<P>() 호출 없음
// server.add_service<MyKafkaService>(parsed.config);
// server.add_adapter<KafkaAdapter>(parsed.kafka);
// server.run();
```
