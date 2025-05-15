#pragma once

#include "adbfsm/async/as_expected.hpp"

#include <boost/asio.hpp>
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

    namespace this_coro = boost::asio::this_coro;
    namespace operators = boost::asio::experimental::awaitable_operators;

    namespace unix_socket
    {
        using Proto    = boost::asio::local::stream_protocol;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    }
}
