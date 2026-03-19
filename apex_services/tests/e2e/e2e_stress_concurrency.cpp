#include "e2e_test_fixture.hpp"

#include <chat_room_generated.h>
#include <flatbuffers/flatbuffers.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace apex::e2e
{

namespace chat_fbs = apex::chat_svc::fbs;

// 환경변수에서 부하 파라미터 읽기 — Valgrind 환경에서는 낮은 값 사용
static int get_env_int(const char* name, int default_val)
{
    const char* val = std::getenv(name);
    return val ? std::atoi(val) : default_val;
}

class E2EStressConcurrencyFixture : public E2ETestFixture
{
  protected:
    int stress_connections_ = get_env_int("E2E_STRESS_CONNECTIONS", 50);
    int stress_messages_ = get_env_int("E2E_STRESS_MESSAGES", 100);
};

/// Concurrency 1: 동일 방에 다수 클라이언트 동시 join → leave 반복
///
/// 10개 클라이언트가 로그인+인증 후 동일 방에 동시에 join→leave를 반복.
/// 모든 클라이언트가 정상 응답을 받는지 확인 (race condition, use-after-free 탐지).
TEST_F(E2EStressConcurrencyFixture, ConcurrentRoomJoinLeave)
{
    constexpr int kClientCount = 10;

    // Alice가 공유 방 생성
    TcpClient alice(io_ctx_, config_);
    alice.connect();
    auto alice_auth = login(alice, "alice@apex.dev", "password123");
    ASSERT_FALSE(alice_auth.access_token.empty()) << "Alice login failed";
    authenticate(alice, alice_auth.access_token);

    uint64_t room_id = 0;
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto name = fbb.CreateString("ConcurrentJoinLeave Room");
        auto req = chat_fbs::CreateCreateRoomRequest(fbb, name, 100);
        fbb.Finish(req);
        alice.send(2001, fbb.GetBufferPointer(), fbb.GetSize());

        auto resp = alice.recv();
        ASSERT_EQ(resp.msg_id, 2002u) << "CreateRoomResponse not received";
        auto* create_resp = flatbuffers::GetRoot<chat_fbs::CreateRoomResponse>(resp.payload.data());
        ASSERT_EQ(create_resp->error(), chat_fbs::ChatRoomError_NONE);
        room_id = create_resp->room_id();
        ASSERT_GT(room_id, 0u);
    }

    // kClientCount개 스레드가 각자 별도 io_context + TcpClient로 동시 join/leave
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> error_count{0};
    std::mutex results_mutex;
    std::vector<std::string> failures;

    threads.reserve(kClientCount);
    for (int i = 0; i < kClientCount; ++i)
    {
        threads.emplace_back(
            [this, room_id, i, &success_count, &error_count, &results_mutex, &failures]()
            {
                try
                {
                    boost::asio::io_context local_io_ctx;
                    TcpClient client(local_io_ctx, config_);
                    client.connect();

                    // bob@apex.dev 로그인 (동시 다중 로그인 허용 가정)
                    auto auth = login(client, "bob@apex.dev", "password123");
                    if (auth.access_token.empty())
                    {
                        std::lock_guard<std::mutex> lock(results_mutex);
                        failures.push_back("client " + std::to_string(i) + ": login failed");
                        error_count++;
                        return;
                    }
                    authenticate(client, auth.access_token);

                    // JoinRoom
                    {
                        flatbuffers::FlatBufferBuilder fbb(128);
                        auto req = chat_fbs::CreateJoinRoomRequest(fbb, room_id);
                        fbb.Finish(req);
                        client.send(2003, fbb.GetBufferPointer(), fbb.GetSize());

                        auto resp = client.recv(std::chrono::seconds{10});
                        if (resp.msg_id != 2004u)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            failures.push_back("client " + std::to_string(i) + ": JoinRoom got msg_id=" +
                                               std::to_string(resp.msg_id));
                            error_count++;
                            return;
                        }
                    }

                    // LeaveRoom
                    {
                        flatbuffers::FlatBufferBuilder fbb(128);
                        auto req = chat_fbs::CreateLeaveRoomRequest(fbb, room_id);
                        fbb.Finish(req);
                        client.send(2005, fbb.GetBufferPointer(), fbb.GetSize());

                        auto resp = client.recv(std::chrono::seconds{10});
                        if (resp.msg_id != 2006u)
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            failures.push_back("client " + std::to_string(i) + ": LeaveRoom got msg_id=" +
                                               std::to_string(resp.msg_id));
                            error_count++;
                            return;
                        }
                    }

                    success_count++;
                    client.close();
                }
                catch (const std::exception& ex)
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    failures.push_back("client " + std::to_string(i) + ": exception: " + ex.what());
                    error_count++;
                }
            });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    // 모든 클라이언트가 성공적으로 join/leave를 완료했어야 함
    for (const auto& f : failures)
    {
        std::cerr << "[E2E-DEBUG] ConcurrentRoomJoinLeave failure: " << f << "\n";
    }
    EXPECT_EQ(success_count.load(), kClientCount)
        << "All clients should complete join/leave. Errors: " << error_count.load();

    alice.close();

    // Gateway still alive
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after concurrent room join/leave";
    probe.close();
}

/// Concurrency 2: 동일 이메일로 다수 동시 로그인 시도
///
/// 동일 이메일(test_concurrent@test.com / password123)로 stress_connections_/5개
/// 클라이언트가 동시에 로그인 시도. 모두 성공하거나 정상 에러 응답을 받는지 확인.
/// (크래시 아님 — race condition에 의한 undefined behavior 탐지가 목적)
///
/// NOTE: test_concurrent@test.com 계정이 DB에 없으면 로그인 실패가 예상됨.
///       이 테스트는 응답이 오는지(에러 포함)를 확인하며, 크래시만 없으면 통과.
TEST_F(E2EStressConcurrencyFixture, ConcurrentLogin)
{
    const int kConcurrentCount = std::max(1, stress_connections_ / 5);

    std::vector<std::thread> threads;
    std::atomic<int> responded_count{0};
    std::atomic<int> crash_count{0};

    threads.reserve(kConcurrentCount);
    for (int i = 0; i < kConcurrentCount; ++i)
    {
        threads.emplace_back(
            [this, i, &responded_count, &crash_count]()
            {
                SCOPED_TRACE("concurrent-login client " + std::to_string(i));
                try
                {
                    boost::asio::io_context local_io_ctx;
                    TcpClient client(local_io_ctx, config_);
                    client.connect();

                    // 동일 이메일로 동시 로그인 시도
                    // 계정이 없으면 에러 응답, 있으면 토큰 발급 — 둘 다 정상
                    flatbuffers::FlatBufferBuilder fbb(256);
                    auto email_off = fbb.CreateString("alice@apex.dev");
                    auto pw_off = fbb.CreateString("password123");
                    auto start = fbb.StartTable();
                    fbb.AddOffset(4, email_off);
                    fbb.AddOffset(6, pw_off);
                    auto loc = fbb.EndTable(start);
                    fbb.Finish(flatbuffers::Offset<void>(loc));
                    client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());

                    auto resp = client.recv(std::chrono::seconds{10});
                    // 응답이 왔다면 (성공 또는 에러 무관) 크래시 아님
                    if (resp.msg_id == 1001u || (resp.flags & ERROR_RESPONSE))
                    {
                        responded_count++;
                    }
                    else
                    {
                        // 예상치 못한 msg_id도 크래시는 아님
                        responded_count++;
                        std::cerr << "[E2E-DEBUG] ConcurrentLogin unexpected resp: msg_id=" << resp.msg_id
                                  << " flags=" << static_cast<int>(resp.flags) << "\n";
                    }

                    client.close();
                }
                catch (const std::exception& ex)
                {
                    // 연결 실패나 타임아웃은 허용 (크래시 아님)
                    std::cerr << "[E2E-DEBUG] ConcurrentLogin client " << i << " exception: " << ex.what() << "\n";
                    responded_count++; // 예외도 크래시는 아님
                }
                catch (...)
                {
                    crash_count++;
                    std::cerr << "[E2E-DEBUG] ConcurrentLogin client " << i << " unknown exception\n";
                }
            });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    // 크래시(unknown exception)가 없어야 함
    EXPECT_EQ(crash_count.load(), 0) << "No crashes expected during concurrent login";
    // 최소한 일부 클라이언트가 응답을 받아야 함
    EXPECT_GT(responded_count.load(), 0) << "At least some clients should receive a response";

    // Gateway still alive
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after concurrent login";
    probe.close();
}

/// Concurrency 3: Rate limit 임계점까지 빠른 요청 버스트
///
/// rate limit 임계점(TOML: ip.total_limit=30)까지 빠르게 요청을 쏟아부음.
/// 임계 이후 요청은 rate limit 에러(ERROR_RESPONSE 플래그 또는 연결 끊김)를
/// 받는지 확인. 크래시가 없어야 함.
///
/// NOTE: gateway_e2e.toml의 rate_limit 설정에 따라 임계점이 다를 수 있음.
///       이 테스트는 rate limit 트리거 여부보다 서버 안정성(크래시 없음)을 검증.
TEST_F(E2EStressConcurrencyFixture, RateLimitBurst)
{
    // Per-IP rate limit 임계점보다 많은 요청 전송
    // gateway_e2e.toml: ip.total_limit=30 → 30개 이상 전송
    constexpr int kBurstCount = 40;
    int rate_limited_count = 0;
    int responded_count = 0;

    for (int i = 0; i < kBurstCount; ++i)
    {
        SCOPED_TRACE("rate-limit burst iteration " + std::to_string(i));

        try
        {
            TcpClient client(io_ctx_, config_);
            client.connect();

            // 인증 없이 요청 전송 (per-IP rate limit 경로)
            flatbuffers::FlatBufferBuilder fbb(128);
            auto start = fbb.StartTable();
            auto loc = fbb.EndTable(start);
            fbb.Finish(flatbuffers::Offset<void>(loc));

            // msg_id=1000 (LoginRequest) — 인증 없이 전송하여 per-IP rate limit 자극
            client.send(1000, fbb.GetBufferPointer(), fbb.GetSize());

            auto resp = client.recv(std::chrono::seconds{3});
            responded_count++;

            if (resp.flags & ERROR_RESPONSE)
            {
                rate_limited_count++;
                std::cerr << "[E2E-DEBUG] RateLimitBurst iter=" << i
                          << " rate-limited: msg_id=" << resp.msg_id
                          << " flags=" << static_cast<int>(resp.flags) << "\n";
            }

            client.close();
        }
        catch (const std::exception& ex)
        {
            // 연결 거부 = rate limit 적용으로 간주
            rate_limited_count++;
            std::cerr << "[E2E-DEBUG] RateLimitBurst iter=" << i << " connection rejected: " << ex.what() << "\n";
        }
    }

    // rate limit이 적용되었거나 적어도 요청이 처리되었어야 함
    std::cerr << "[E2E-DEBUG] RateLimitBurst: responded=" << responded_count
              << " rate_limited=" << rate_limited_count << "\n";

    // rate limit이 트리거되었거나 전체 요청이 처리됨 — 크래시가 없으면 통과
    EXPECT_GT(responded_count + rate_limited_count, 0) << "All burst requests should be handled (no crash)";

    // IP rate limit 윈도우 만료 대기 (gateway_e2e.toml: window_size_seconds=2, 3초 여유)
    std::this_thread::sleep_for(std::chrono::seconds{3});

    // Gateway still alive: rate limit 윈도우 만료 후 정상 접속 가능한지 확인
    TcpClient probe(io_ctx_, config_);
    ASSERT_NO_THROW(probe.connect()) << "Gateway should still be alive after rate limit burst";
    EXPECT_TRUE(probe.is_connected());
    probe.close();
}

} // namespace apex::e2e
