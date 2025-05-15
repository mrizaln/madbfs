#pragma once

#include "adbfsm/async/as_expected.hpp"

#include <boost/asio.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <expected>
#include <utility>

namespace adbfsm::async
{
    using Context  = boost::asio::io_context;
    using Token    = AsExpected<boost::asio::use_awaitable_t<>>;
    using Executor = Context::executor_type;

    template <typename T>
    using Await = boost::asio::awaitable<T>;

    template <typename Exec, typename Awaited, typename Completion>
    auto spawn(Exec&& ex, Awaited&& func, Completion&& completion)
    {
        return co_spawn(
            std::forward<Exec>(ex),    //
            std::forward<Awaited>(func),
            std::forward<Completion>(completion)
        );
    }

    inline std::errc to_generic_err(boost::system::error_code ec, std::errc fallback = std::errc::io_error)
    {
        if (ec.category() == boost::system::generic_category()) {
            return static_cast<std::errc>(ec.value());
        }
        return static_cast<bool>(fallback) ? fallback : std::errc::invalid_argument;
    }

    template <typename AStream>
    Await<std::pair<boost::system::error_code, std::size_t>> write_all(AStream& stream, std::string_view buf)
    {
        auto written = 0uz;
        while (written < buf.size()) {
            auto buffer = boost::asio::buffer(buf.data() + written, buf.size() - written);
            auto res    = co_await stream.async_write_some(buffer);
            if (not res) {
                co_return std::pair{ res.error(), written };
            }
            auto len = *res;
            if (len == 0) {
                co_return std::pair{ std::make_error_code(std::errc::broken_pipe), written };
            }
            written += len;
        }
        co_return std::pair{ std::error_code{}, written };
    }

    template <typename AStream>
    Await<std::pair<boost::system::error_code, std::size_t>> read_all(AStream& stream, std::string& buf)
    {
        auto dyn_buf = boost::asio::dynamic_buffer(buf);
        while (true) {
            auto res = co_await stream.async_read_some(dyn_buf);
            if (res) {
                continue;
            }
            if (res.error() == boost::asio::error::eof) {
                break;
            }
            co_return std::pair{ res.error(), dyn_buf.size() };
        }
        co_return std::pair{ std::error_code{}, dyn_buf.size() };
    }

    using boost::asio::buffer;
    using boost::asio::dynamic_buffer;

    namespace this_coro = boost::asio::this_coro;
    namespace operators = boost::asio::experimental::awaitable_operators;

    namespace unix_socket
    {
        using Proto    = boost::asio::local::stream_protocol;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    }

    namespace pipe
    {
        using Write = Token::as_default_on_t<boost::asio::writable_pipe>;
        using Read  = Token::as_default_on_t<boost::asio::readable_pipe>;
    }
}
