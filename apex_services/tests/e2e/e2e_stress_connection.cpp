#include "e2e_test_fixture.hpp"

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>
#include <vector>

namespace apex::e2e
{

// 환경변수에서 부하 파라미터 읽기 — Valgrind 환경에서는 낮은 값 사용
static int get_env_int(const char* name, int default_val)
{
    const char* val = std::getenv(name);
    return val ? std::atoi(val) : default_val;
}

class E2EStressConnectionFixture : public E2ETestFixture
{
  protected:
    int stress_connections_ = get_env_int("E2E_STRESS_CONNECTIONS", 50);
};

/// Stress 1: 순차 connect → disconnect 반복
///
/// stress_connections_개 클라이언트를 순차적으로 접속 후 즉시 disconnect.
/// 각 클라이언트가 정상 종료되고, 마지막에 새 클라이언트가 Gateway에 접속
/// 가능한지 확인 (Gateway 크래시/리소스 고갈 없음 검증).
TEST_F(E2EStressConnectionFixture, RapidConnectDisconnect)
{
    for (int i = 0; i < stress_connections_; ++i)
    {
        SCOPED_TRACE("iteration " + std::to_string(i));

        TcpClient client(io_ctx_, config_);
        client.connect();
        EXPECT_TRUE(client.is_connected());
        client.close();
        EXPECT_FALSE(client.is_connected());
    }

    // Gateway still alive: 새 클라이언트로 접속 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still accept connections after rapid connect/disconnect";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

/// Stress 2: Half-open connection — 응답 받기 전 소켓 강제 종료
///
/// LoginRequest(msg_id=1000) 전송 후 응답이 오기 전에 소켓을 close()로
/// 강제 종료. Gateway가 응답 전송 중 연결 끊김을 처리하면서 크래시하지
/// 않는지 검증. 이후 새 클라이언트로 Gateway 생존을 확인.
TEST_F(E2EStressConnectionFixture, HalfOpenConnection)
{
    constexpr int kHalfOpenCount = 10;

    for (int i = 0; i < kHalfOpenCount; ++i)
    {
        SCOPED_TRACE("half-open iteration " + std::to_string(i));

        TcpClient client(io_ctx_, config_);
        client.connect();

        // LoginRequest 전송 (msg_id=1000)
        {
            flatbuffers::FlatBufferBuilder fbb(256);
            auto email_off = fbb.CreateString("alice@apex.dev");
            auto pw_off = fbb.CreateString("password123");
            auto start = fbb.StartTable();
            fbb.AddOffset(4, email_off);
            fbb.AddOffset(6, pw_off);
            auto loc = fbb.EndTable(start);
            fbb.Finish(flatbuffers::Offset<void>(loc));
            client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());
        }

        // 응답 수신 전에 즉시 소켓 강제 종료
        client.close();
    }

    // 짧은 대기로 Gateway가 비동기 write 실패를 처리할 시간 확보
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    // Gateway still alive: 새 클라이언트로 정상 로그인 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after half-open connections";
    auto auth = login(probe, "alice@apex.dev", "password123");
    EXPECT_FALSE(auth.access_token.empty()) << "Gateway should still handle login after half-open connections";
    probe.close();
}

/// Stress 3: 로그인 요청 전송 직후 즉시 close
///
/// LoginRequest(msg_id=1000) 전송 직후 응답을 기다리지 않고 바로 close().
/// 서버가 응답 경로를 찾지 못한 상태에서 write를 시도할 때 크래시가
/// 발생하지 않는지 검증.
TEST_F(E2EStressConnectionFixture, DisconnectDuringResponse)
{
    constexpr int kDisconnectCount = 10;

    for (int i = 0; i < kDisconnectCount; ++i)
    {
        SCOPED_TRACE("disconnect-during-response iteration " + std::to_string(i));

        TcpClient client(io_ctx_, config_);
        client.connect();

        // LoginRequest 전송 후 즉시 close — 응답이 오기 전에 연결 종료
        {
            flatbuffers::FlatBufferBuilder fbb(256);
            auto email_off = fbb.CreateString("bob@apex.dev");
            auto pw_off = fbb.CreateString("password123");
            auto start = fbb.StartTable();
            fbb.AddOffset(4, email_off);
            fbb.AddOffset(6, pw_off);
            auto loc = fbb.EndTable(start);
            fbb.Finish(flatbuffers::Offset<void>(loc));
            client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());
        }

        // 소켓 즉시 강제 종료 (응답 대기 없음)
        client.close();
    }

    // Gateway가 고아 응답 처리 중 서버 측 오류를 회복할 시간 확보
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    // Gateway still alive: 정상 클라이언트로 접속 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should not crash after disconnect-during-response";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

} // namespace apex::e2e
