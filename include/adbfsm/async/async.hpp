#pragma once

#include "adbfsm/async/as_expected.hpp"
#include "adbfsm/common.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <expected>
#include <utility>

namespace adbfsm
{
    template <typename T>
    using Await = boost::asio::awaitable<T>;

    template <typename T>
    using AExpect = Await<Expect<T>>;
}

namespace adbfsm::async
{
    using Context  = boost::asio::io_context;
    using Token    = AsExpected<boost::asio::use_awaitable_t<>>;
    using Executor = Context::executor_type;

    template <typename Exec, typename Awaited, typename Completion>
    auto spawn(Exec&& ex, Awaited&& func, Completion&& completion)
    {
        return boost::asio::co_spawn(
            std::forward<Exec>(ex), std::forward<Awaited>(func), std::forward<Completion>(completion)
        );
    }

    inline Errc to_generic_err(boost::system::error_code ec, Errc fallback = Errc::io_error)
    {
        if (ec.category() == boost::system::generic_category()) {
            return static_cast<Errc>(ec.value());
        }
        return static_cast<bool>(fallback) ? fallback : Errc::invalid_argument;
    }

    template <typename AStream>
    Await<Pair<boost::system::error_code, usize>> write_exact(AStream& stream, Span<const char> in)
    {
        auto written = 0uz;
        while (written < in.size()) {
            auto buffer = boost::asio::buffer(in.data() + written, in.size() - written);
            auto res    = co_await stream.async_write_some(buffer);
            if (not res) {
                co_return Pair{ res.error(), written };
            }
            auto len = *res;
            if (len == 0) {
                co_return Pair{ std::make_error_code(Errc::broken_pipe), written };
            }
            written += len;
        }
        co_return Pair{ std::error_code{}, written };
    }

    template <typename AStream>
    Await<Pair<boost::system::error_code, usize>> read_exact(AStream& stream, Span<char> out)
    {
        auto read = 0uz;
        while (read < out.size()) {
            auto buffer = boost::asio::buffer(out.data() + read, out.size() - read);
            auto res    = co_await stream.async_read_some(buffer);
            if (not res) {
                co_return Pair{ res.error(), read };
            }
            auto len = *res;
            if (len == 0) {
                co_return Pair{ std::make_error_code(Errc::broken_pipe), read };
            }
            read += len;
        }
        co_return Pair{ std::error_code{}, read };
    }

    using boost::asio::buffer;
    using boost::asio::dynamic_buffer;

    using boost::asio::detached;
    using boost::asio::use_awaitable;
    using boost::asio::use_future;

    namespace this_coro = boost::asio::this_coro;
    namespace operators = boost::asio::experimental::awaitable_operators;

    namespace unix_socket
    {
        using Proto    = boost::asio::local::stream_protocol;
        using Endpoint = Proto::endpoint;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    }

    namespace pipe
    {
        using Write = Token::as_default_on_t<boost::asio::writable_pipe>;
        using Read  = Token::as_default_on_t<boost::asio::readable_pipe>;
    }
}
