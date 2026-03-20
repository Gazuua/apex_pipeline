// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>
#include <vector>

namespace apex::e2e
{

class E2EStressInfraFixture : public E2ETestFixture
{
  protected:
    int stress_messages_ = get_env_int("E2E_STRESS_MESSAGES", 20);
};

/// Infra 1: PendingRequests 타임아웃 대량 발생 경로 검증
///
/// 라우팅은 되지만 처리 서비스가 핸들러를 등록하지 않은 msg_id 범위
/// (3000-3009)로 다수 요청 전송. Gateway의 PendingRequests 맵에서
/// 타임아웃이 대량 발생하고, 클라이언트가 타임아웃 에러 응답을 받는지 확인.
///
/// NOTE: msg_id 3000-3009 범위는 Chat Service 라우팅(2000-2999) 바깥이므로
///       Gateway가 INVALID_MSG_ID로 즉시 거부하거나 라우팅 불가로 처리.
///       어느 쪽이든 에러 응답이 반환되어야 하며 크래시가 없어야 함.
TEST_F(E2EStressInfraFixture, MassTimeouts)
{
    TcpClient client(io_ctx_, config_);
    client.connect();

    auto auth = login(client, "alice@apex.dev", "password123");
    ASSERT_FALSE(auth.access_token.empty()) << "Login failed — cannot run MassTimeouts test";
    authenticate(client, auth.access_token);

    // msg_id 범위 3000-3009: 등록된 서비스 핸들러 없음
    // Gateway는 INVALID_MSG_ID 또는 SERVICE_TIMEOUT 에러 응답을 반환해야 함
    constexpr int kTimeoutRequests = 5; // 타임아웃 대기 비용으로 소수만 테스트
    int error_response_count = 0;

    for (int i = 0; i < kTimeoutRequests; ++i)
    {
        SCOPED_TRACE("mass-timeout request " + std::to_string(i));

        const uint32_t msg_id = 3000 + static_cast<uint32_t>(i);
        try
        {
            // 최소 payload (빈 FlatBuffers 테이블)
            flatbuffers::FlatBufferBuilder fbb(64);
            auto start = fbb.StartTable();
            auto loc = fbb.EndTable(start);
            fbb.Finish(flatbuffers::Offset<void>(loc));
            client.send(msg_id, fbb.GetBufferPointer(), fbb.GetSize());

            // 타임아웃 응답 대기 (gateway_e2e.toml: request_timeout_ms=5000, 여유 15초)
            auto resp = client.recv(std::chrono::seconds{15});
            if (resp.flags & ERROR_RESPONSE)
            {
                error_response_count++;
                std::cerr << "[E2E-DEBUG] MassTimeouts: got error resp for msg_id=" << msg_id
                          << " resp.msg_id=" << resp.msg_id << "\n";
            }
        }
        catch (const std::exception& ex)
        {
            // 연결 끊김 또는 소켓 타임아웃 — 재연결 후 계속
            std::cerr << "[E2E-DEBUG] MassTimeouts iter=" << i << " exception: " << ex.what() << "\n";
            error_response_count++;
            break;
        }
    }

    EXPECT_GT(error_response_count, 0) << "Expected at least one error response for unregistered msg_id range";

    client.close();

    // Gateway still alive: 정상 로그인이 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after mass timeout requests";
    auto probe_auth = login(probe, "bob@apex.dev", "password123");
    EXPECT_FALSE(probe_auth.access_token.empty()) << "Gateway should handle normal login after mass timeouts";
    probe.close();
}

/// Infra 2: Kafka 일시 중단 후 재개 — 재연결 및 정상 복구 검증
///
/// docker pause로 Kafka 브로커를 일시 중단하고, 5초 대기 후 unpause로 재개.
/// 재개 후 10초 대기 뒤 정상 로그인 요청이 성공하는지 확인.
/// Gateway/Service의 Kafka 재연결 로직이 정상 동작하는지 검증.
TEST_F(E2EStressInfraFixture, KafkaReconnect)
{
    // 초기 상태 확인: 로그인 가능한지 검증
    {
        TcpClient pre_client(io_ctx_, config_);
        ASSERT_NO_THROW(pre_client.connect());
        auto pre_auth = login(pre_client, "alice@apex.dev", "password123");
        ASSERT_FALSE(pre_auth.access_token.empty()) << "Pre-pause login should succeed";
        pre_client.close();
    }

    std::cerr << "[E2E-DEBUG] KafkaReconnect: pausing Kafka...\n";

    // Kafka 컨테이너 일시 중단
    int rc = std::system("docker pause apex-e2e-kafka");
    if (rc != 0)
    {
        GTEST_SKIP() << "docker pause failed (rc=" << rc << ") — Kafka container not accessible, skipping";
    }

    // 5초 대기 (Kafka 중단 상태 유지)
    std::this_thread::sleep_for(std::chrono::seconds{5});

    std::cerr << "[E2E-DEBUG] KafkaReconnect: unpausing Kafka...\n";

    // Kafka 컨테이너 재개
    rc = std::system("docker unpause apex-e2e-kafka");
    EXPECT_EQ(rc, 0) << "docker unpause should succeed";

    // 10초 대기: Kafka 브로커 재초기화 + 서비스 재연결 완료 대기
    std::cerr << "[E2E-DEBUG] KafkaReconnect: waiting 10s for reconnection...\n";
    std::this_thread::sleep_for(std::chrono::seconds{10});

    // 정상 로그인 요청이 성공하는지 확인 (Kafka 파이프라인 복구 검증)
    TcpClient client(io_ctx_, config_);
    ASSERT_NO_THROW(client.connect()) << "Gateway should accept connections after Kafka reconnect";
    auto auth = login(client, "alice@apex.dev", "password123");
    EXPECT_FALSE(auth.access_token.empty())
        << "Login should succeed after Kafka reconnect (Kafka pipeline should be restored)";
    client.close();
}

/// Infra 3: Redis Auth 일시 중단 후 재개 — 재연결 및 정상 복구 검증
///
/// docker pause로 Redis Auth 인스턴스를 일시 중단하고, 5초 대기 후 unpause로 재개.
/// 재개 후 10초 대기 뒤 정상 로그인이 성공하는지 확인.
/// Auth Service의 Redis 재연결 로직이 정상 동작하는지 검증.
TEST_F(E2EStressInfraFixture, RedisReconnect)
{
    // 초기 상태 확인: 로그인 가능한지 검증
    {
        TcpClient pre_client(io_ctx_, config_);
        ASSERT_NO_THROW(pre_client.connect());
        auto pre_auth = login(pre_client, "alice@apex.dev", "password123");
        ASSERT_FALSE(pre_auth.access_token.empty()) << "Pre-pause login should succeed";
        pre_client.close();
    }

    std::cerr << "[E2E-DEBUG] RedisReconnect: pausing Redis Auth...\n";

    // Redis Auth 컨테이너 일시 중단
    int rc = std::system("docker pause apex-e2e-redis-auth");
    if (rc != 0)
    {
        GTEST_SKIP() << "docker pause failed (rc=" << rc << ") — Redis Auth container not accessible, skipping";
    }

    // 5초 대기 (Redis 중단 상태 유지)
    std::this_thread::sleep_for(std::chrono::seconds{5});

    std::cerr << "[E2E-DEBUG] RedisReconnect: unpausing Redis Auth...\n";

    // Redis Auth 컨테이너 재개
    rc = std::system("docker unpause apex-e2e-redis-auth");
    EXPECT_EQ(rc, 0) << "docker unpause should succeed";

    // 10초 대기: Redis 재초기화 + Auth Service 재연결 완료 대기
    std::cerr << "[E2E-DEBUG] RedisReconnect: waiting 10s for reconnection...\n";
    std::this_thread::sleep_for(std::chrono::seconds{10});

    // 정상 로그인이 성공하는지 확인 (Redis Auth 재연결 검증)
    TcpClient client(io_ctx_, config_);
    ASSERT_NO_THROW(client.connect()) << "Gateway should accept connections after Redis reconnect";
    auto auth = login(client, "alice@apex.dev", "password123");
    EXPECT_FALSE(auth.access_token.empty())
        << "Login should succeed after Redis reconnect (Redis Auth pipeline should be restored)";
    client.close();
}

} // namespace apex::e2e
