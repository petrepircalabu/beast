//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef BOOST_BEAST_WEBSOCKET_IMPL_CLOSE_IPP
#define BOOST_BEAST_WEBSOCKET_IMPL_CLOSE_IPP

#include <boost/beast/websocket/teardown.hpp>
#include <boost/beast/core/handler_ptr.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/core/type_traits.hpp>
#include <boost/beast/core/detail/config.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/handler_alloc_hook.hpp>
#include <boost/asio/handler_continuation_hook.hpp>
#include <boost/asio/handler_invoke_hook.hpp>
#include <boost/throw_exception.hpp>
#include <memory>

namespace boost {
namespace beast {
namespace websocket {

//------------------------------------------------------------------------------

/*  Close the WebSocket Connection

    This composed operation sends the close frame if it hasn't already
    been sent, then reads and discards frames until receiving a close
    frame. Finally it invokes the teardown operation to shut down the
    underlying connection.
*/
template<class NextLayer>
template<class Handler>
class stream<NextLayer>::close_op
    : public boost::asio::coroutine
{
    struct state
    {
        stream<NextLayer>& ws;
        detail::frame_buffer fb;
        error_code ev;
        token tok;

        state(
            Handler&,
            stream<NextLayer>& ws_,
            close_reason const& cr)
            : ws(ws_)
            , tok(ws.tok_.unique())
        {
            // Serialize the close frame
            ws.template write_close<
                flat_static_buffer_base>(fb, cr);
        }
    };

    handler_ptr<state, Handler> d_;

public:
    close_op(close_op&&) = default;
    close_op(close_op const&) = default;

    template<class DeducedHandler>
    close_op(
        DeducedHandler&& h,
        stream<NextLayer>& ws,
        close_reason const& cr)
        : d_(std::forward<DeducedHandler>(h), ws, cr)
    {
    }

    void
    operator()(
        error_code ec = {},
        std::size_t bytes_transferred = 0);

    friend
    void* asio_handler_allocate(
        std::size_t size, close_op* op)
    {
        using boost::asio::asio_handler_allocate;
        return asio_handler_allocate(
            size, std::addressof(op->d_.handler()));
    }

    friend
    void asio_handler_deallocate(
        void* p, std::size_t size, close_op* op)
    {
        using boost::asio::asio_handler_deallocate;
        asio_handler_deallocate(
            p, size, std::addressof(op->d_.handler()));
    }

    friend
    bool asio_handler_is_continuation(close_op* op)
    {
        using boost::asio::asio_handler_is_continuation;
        return asio_handler_is_continuation(
            std::addressof(op->d_.handler()));
    }

    template<class Function>
    friend
    void asio_handler_invoke(Function&& f, close_op* op)
    {
        using boost::asio::asio_handler_invoke;
        asio_handler_invoke(
            f, std::addressof(op->d_.handler()));
    }
};

template<class NextLayer>
template<class Handler>
void
stream<NextLayer>::close_op<Handler>::
operator()(error_code ec, std::size_t bytes_transferred)
{
    using beast::detail::clamp;
    auto& d = *d_;
    close_code code{};
    BOOST_ASIO_CORO_REENTER(*this)
    {
        // Maybe suspend
        if(! d.ws.wr_block_)
        {
            // Acquire the write block
            d.ws.wr_block_ = d.tok;

            // Make sure the stream is open
            if(! d.ws.open_)
            {
                BOOST_ASIO_CORO_YIELD
                d.ws.get_io_service().post(
                    bind_handler(std::move(*this),
                        boost::asio::error::operation_aborted));
                goto upcall;
            }
        }
        else
        {
            // Suspend
            BOOST_ASSERT(d.ws.wr_block_ != d.tok);
            BOOST_ASIO_CORO_YIELD
            d.ws.paused_close_.emplace(std::move(*this));

            // Acquire the write block
            BOOST_ASSERT(! d.ws.wr_block_);
            d.ws.wr_block_ = d.tok;

            // Resume
            BOOST_ASIO_CORO_YIELD
            d.ws.get_io_service().post(std::move(*this));
            BOOST_ASSERT(d.ws.wr_block_ == d.tok);

            // Make sure the stream is open
            if(! d.ws.open_)
            {
                ec = boost::asio::error::operation_aborted;
                goto upcall;
            }
        }

        // Send close frame
        BOOST_ASSERT(! d.ws.wr_close_);
        d.ws.wr_close_ = true;
        BOOST_ASIO_CORO_YIELD
        boost::asio::async_write(d.ws.stream_,
            d.fb.data(), std::move(*this));
        if(ec)
        {
            d.ws.open_ = false;
            goto upcall;
        }

        if(d.ws.rd_close_)
        {
            // This happens when the read_op gets a close frame
            // at the same time we are sending the close frame. The
            // read_op will be suspended on the write block.
            goto teardown;
        }

        if(! d.ws.rd_block_)
        {
            // Acquire the read block
            d.ws.rd_block_ = d.tok;
        }
        else
        {
            // The read_op is currently running so it will see
            // the close frame and call teardown. We will suspend
            // to cause async_read to return error::closed, before
            // we return error::success.

            // Suspend
            BOOST_ASSERT(d.ws.rd_block_ != d.tok);
            BOOST_ASIO_CORO_YIELD
            d.ws.paused_r_close_.emplace(std::move(*this));

            // Acquire the read block
            BOOST_ASSERT(! d.ws.rd_block_);
            d.ws.rd_block_ = d.tok;

            // Resume
            BOOST_ASIO_CORO_YIELD
            d.ws.get_io_service().post(std::move(*this));
            BOOST_ASSERT(d.ws.rd_block_ == d.tok);

            // Handle the stream closing while suspended
            if(! d.ws.open_)
            {
                ec = boost::asio::error::operation_aborted;
                goto upcall;
            }
        }

        // Drain
        if(d.ws.rd_remain_ > 0)
            goto read_payload;
        for(;;)
        {
            // Read frame header
            while(! d.ws.parse_fh(
                d.ws.rd_fh_, d.ws.rd_buf_, code))
            {
                if(code != close_code::none)
                {
                    d.ev = error::failed;
                    goto teardown;
                }
                BOOST_ASIO_CORO_YIELD
                d.ws.stream_.async_read_some(
                    d.ws.rd_buf_.prepare(read_size(d.ws.rd_buf_,
                        d.ws.rd_buf_.max_size())),
                            std::move(*this));
                if(ec)
                {
                    d.ws.open_ = false;
                    goto upcall;
                }
                d.ws.rd_buf_.commit(bytes_transferred);
            }
            if(detail::is_control(d.ws.rd_fh_.op))
            {
                // Process control frame
                if(d.ws.rd_fh_.op == detail::opcode::close)
                {
                    BOOST_ASSERT(! d.ws.rd_close_);
                    d.ws.rd_close_ = true;
                    auto const mb = buffer_prefix(
                        clamp(d.ws.rd_fh_.len),
                        d.ws.rd_buf_.data());
                    if(d.ws.rd_fh_.len > 0 && d.ws.rd_fh_.mask)
                        detail::mask_inplace(mb, d.ws.rd_key_);
                    detail::read_close(d.ws.cr_, mb, code);
                    if(code != close_code::none)
                    {
                        // Protocol error
                        d.ev = error::failed;
                        goto teardown;
                    }
                    d.ws.rd_buf_.consume(clamp(d.ws.rd_fh_.len));
                    goto teardown;
                }
                d.ws.rd_buf_.consume(clamp(d.ws.rd_fh_.len));
            }
            else
            {
            read_payload:
                while(d.ws.rd_buf_.size() < d.ws.rd_remain_)
                {
                    d.ws.rd_remain_ -= d.ws.rd_buf_.size();
                    d.ws.rd_buf_.consume(d.ws.rd_buf_.size());
                    BOOST_ASIO_CORO_YIELD
                    d.ws.stream_.async_read_some(
                        d.ws.rd_buf_.prepare(read_size(d.ws.rd_buf_,
                            d.ws.rd_buf_.max_size())),
                                std::move(*this));
                    if(ec)
                    {
                        d.ws.open_ = false;
                        goto upcall;
                    }
                    d.ws.rd_buf_.commit(bytes_transferred);
                }
                BOOST_ASSERT(d.ws.rd_buf_.size() >= d.ws.rd_remain_);
                d.ws.rd_buf_.consume(clamp(d.ws.rd_remain_));
                d.ws.rd_remain_ = 0;
            }
        }

    teardown:
        // Teardown
        BOOST_ASSERT(d.ws.wr_block_ == d.tok);
        using beast::websocket::async_teardown;
        BOOST_ASIO_CORO_YIELD
        async_teardown(d.ws.role_,
            d.ws.stream_, std::move(*this));
        BOOST_ASSERT(d.ws.wr_block_ == d.tok);
        if(ec == boost::asio::error::eof)
        {
            // Rationale:
            // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
            ec.assign(0, ec.category());
        }
        if(! ec)
            ec = d.ev;
        d.ws.open_ = false;

    upcall:
        BOOST_ASSERT(d.ws.wr_block_ == d.tok);
        d.ws.wr_block_.reset();
        if(d.ws.rd_block_)
        {
            BOOST_ASSERT(d.ws.rd_block_ = d.tok);
            d.ws.rd_block_.reset();
            d.ws.paused_r_rd_.maybe_invoke();
        }
        d.ws.paused_rd_.maybe_invoke() ||
            d.ws.paused_ping_.maybe_invoke() ||
            d.ws.paused_wr_.maybe_invoke();
        d_.invoke(ec);
    }
}

//------------------------------------------------------------------------------

template<class NextLayer>
void
stream<NextLayer>::
close(close_reason const& cr)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream requirements not met");
    error_code ec;
    close(cr, ec);
    if(ec)
        BOOST_THROW_EXCEPTION(system_error{ec});
}

template<class NextLayer>
void
stream<NextLayer>::
close(close_reason const& cr, error_code& ec)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream requirements not met");
    using beast::detail::clamp;
    // Make sure the stream is open
    if(! open_)
    {
        ec = boost::asio::error::operation_aborted;
        return;
    }
    // If rd_close_ is set then we already sent a close
    BOOST_ASSERT(! rd_close_);
    BOOST_ASSERT(! wr_close_);
    wr_close_ = true;
    {
        detail::frame_buffer fb;
        write_close<flat_static_buffer_base>(fb, cr);
        boost::asio::write(stream_, fb.data(), ec);
    }
    open_ = ! ec;
    if(! open_)
        return;
    // Drain the connection
    close_code code{};
    if(rd_remain_ > 0)
        goto read_payload;
    for(;;)
    {
        // Read frame header
        while(! parse_fh(rd_fh_, rd_buf_, code))
        {
            if(code != close_code::none)
                return do_fail(close_code::none,
                    error::failed, ec);
            auto const bytes_transferred =
                stream_.read_some(
                    rd_buf_.prepare(read_size(rd_buf_,
                        rd_buf_.max_size())), ec);
            open_ = ! ec;
            if(! open_)
                return;
            rd_buf_.commit(bytes_transferred);
        }
        if(detail::is_control(rd_fh_.op))
        {
            // Process control frame
            if(rd_fh_.op == detail::opcode::close)
            {
                BOOST_ASSERT(! rd_close_);
                rd_close_ = true;
                auto const mb = buffer_prefix(
                    clamp(rd_fh_.len),
                    rd_buf_.data());
                if(rd_fh_.len > 0 && rd_fh_.mask)
                    detail::mask_inplace(mb, rd_key_);
                detail::read_close(cr_, mb, code);
                if(code != close_code::none)
                    // Protocol error
                    return do_fail(close_code::none,
                        error::failed, ec);
                rd_buf_.consume(clamp(rd_fh_.len));
                break;
            }
            rd_buf_.consume(clamp(rd_fh_.len));
        }
        else
        {
        read_payload:
            while(rd_buf_.size() < rd_remain_)
            {
                rd_remain_ -= rd_buf_.size();
                rd_buf_.consume(rd_buf_.size());
                auto const bytes_transferred =
                    stream_.read_some(
                        rd_buf_.prepare(read_size(rd_buf_,
                            rd_buf_.max_size())), ec);
                open_ = ! ec;
                if(! open_)
                    return;
                rd_buf_.commit(bytes_transferred);
            }
            BOOST_ASSERT(rd_buf_.size() >= rd_remain_);
            rd_buf_.consume(clamp(rd_remain_));
            rd_remain_ = 0;
        }
    }
    // _Close the WebSocket Connection_
    do_fail(close_code::none, error::closed, ec);
    if(ec == error::closed)
        ec.assign(0, ec.category());
}

template<class NextLayer>
template<class CloseHandler>
async_return_type<
    CloseHandler, void(error_code)>
stream<NextLayer>::
async_close(close_reason const& cr, CloseHandler&& handler)
{
    static_assert(is_async_stream<next_layer_type>::value,
        "AsyncStream requirements not met");
    async_completion<CloseHandler,
        void(error_code)> init{handler};
    close_op<handler_type<
        CloseHandler, void(error_code)>>{
            init.completion_handler, *this, cr}({});
    return init.result.get();
}

} // websocket
} // beast
} // boost

#endif
