// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/redis/hiredis_asio_adapter.hpp>

#include <apex/core/scoped_logger.hpp>

#include <hiredis/async.h>
#include <hiredis/hiredis.h>

namespace apex::shared::adapters::redis
{

namespace
{
const apex::core::ScopedLogger& s_logger()
{
    static const apex::core::ScopedLogger instance{"HiredisAsioAdapter", apex::core::ScopedLogger::NO_CORE, "app"};
    return instance;
}
} // anonymous namespace

HiredisAsioAdapter::HiredisAsioAdapter(boost::asio::io_context& io_ctx, redisAsyncContext* ac)
    : socket_(io_ctx)
    , ac_(ac)
{
    // fd를 Asio 소켓에 assign.
    // Windows: redisFD는 Winsock SOCKET (unsigned long long) — IOCP에 등록됨.
    // Linux: redisFD는 int — epoll에 등록됨.
    auto native_fd = ac->c.fd;
    socket_.assign(boost::asio::ip::tcp::v4(),
                   static_cast<boost::asio::ip::tcp::socket::native_handle_type>(native_fd));

    // hiredis ev 콜백 등록
    ac->ev.addRead = on_add_read;
    ac->ev.delRead = on_del_read;
    ac->ev.addWrite = on_add_write;
    ac->ev.delWrite = on_del_write;
    ac->ev.cleanup = on_cleanup;
    ac->ev.data = this; // privdata로 this 전달
}

HiredisAsioAdapter::~HiredisAsioAdapter()
{
    if (!cleaned_up_)
    {
        // 소켓을 release하여 fd를 Asio에서 분리하되 close하지 않음.
        // hiredis가 redisAsyncFree()에서 fd를 close하므로 여기서는 release만.
        boost::system::error_code ec;
        socket_.cancel(ec);
        socket_.release(ec);
    }
}

// --- static ev callbacks ---

void HiredisAsioAdapter::on_add_read(void* privdata)
{
    auto* self = static_cast<HiredisAsioAdapter*>(privdata);
    if (self->cleaned_up_)
        return;
    if (!self->read_requested_)
    {
        self->read_requested_ = true;
        self->handle_read();
    }
}

void HiredisAsioAdapter::on_del_read(void* privdata)
{
    auto* self = static_cast<HiredisAsioAdapter*>(privdata);
    self->read_requested_ = false;
}

void HiredisAsioAdapter::on_add_write(void* privdata)
{
    auto* self = static_cast<HiredisAsioAdapter*>(privdata);
    if (self->cleaned_up_)
        return;
    if (!self->write_requested_)
    {
        self->write_requested_ = true;
        self->handle_write();
    }
}

void HiredisAsioAdapter::on_del_write(void* privdata)
{
    auto* self = static_cast<HiredisAsioAdapter*>(privdata);
    self->write_requested_ = false;
}

void HiredisAsioAdapter::on_cleanup(void* privdata)
{
    auto* self = static_cast<HiredisAsioAdapter*>(privdata);
    s_logger().trace("on_cleanup");
    self->cleaned_up_ = true;

    // 소켓 release: fd를 Asio에서 분리하되 close하지 않음
    boost::system::error_code ec;
    self->socket_.cancel(ec);
    self->socket_.release(ec);
}

// --- read/write event handlers ---

void HiredisAsioAdapter::handle_read()
{
    if (!read_requested_ || cleaned_up_)
        return;
    socket_.async_wait(boost::asio::ip::tcp::socket::wait_read, [this](boost::system::error_code ec) {
        if (!ec && read_requested_ && !cleaned_up_)
        {
            redisAsyncHandleRead(ac_);
            handle_read(); // 재등록 (연속 읽기)
        }
    });
}

void HiredisAsioAdapter::handle_write()
{
    if (!write_requested_ || cleaned_up_)
        return;
    socket_.async_wait(boost::asio::ip::tcp::socket::wait_write, [this](boost::system::error_code ec) {
        if (!ec && write_requested_ && !cleaned_up_)
        {
            redisAsyncHandleWrite(ac_);
            // write는 재등록하지 않음 — hiredis가 필요 시 addWrite 다시 호출
        }
    });
}

} // namespace apex::shared::adapters::redis
