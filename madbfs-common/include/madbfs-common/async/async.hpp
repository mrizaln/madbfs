#pragma once

#ifndef MADBFS_NON_BOOST_ASIO
#    define MADBFS_NON_BOOST_ASIO 0
#endif

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/as_expected.hpp"

#if MADBFS_NON_BOOST_ASIO
#    include <asio.hpp>
#    include <asio/error.hpp>
#    include <asio/experimental/awaitable_operators.hpp>
#    include <asio/experimental/coro.hpp>
#else
#    include <boost/asio.hpp>
#    include <boost/asio/error.hpp>
#    include <boost/asio/experimental/awaitable_operators.hpp>
#    include <boost/asio/experimental/coro.hpp>
#endif

#include <atomic>
#include <expected>
#include <utility>

namespace madbfs
{
#if not MADBFS_NON_BOOST_ASIO
    namespace asio   = boost::asio;
    using error_code = boost::system::error_code;
#else
    namespace asio   = ::asio;
    using error_code = std::error_code;
#endif

    template <typename T>
    using Await = asio::awaitable<T>;

    template <typename T, typename E = std::errc>
    using AExpect = Await<Expect<T, E>>;

    template <typename T>
    using AGen = asio::experimental::generator<T>;
}

namespace madbfs::async
{
    using Context   = asio::io_context;
    using Token     = AsExpected<asio::use_awaitable_t<>>;
    using Executor  = Context::executor_type;
    using WorkGuard = asio::executor_work_guard<Executor>;

    using Timer = Token::as_default_on_t<asio::steady_timer>;

    template <typename Exec, typename Awaited, typename Completion>
    auto spawn(Exec&& ex, Awaited&& func, Completion&& completion) noexcept
    {
        return asio::co_spawn(
            std::forward<Exec>(ex), std::forward<Awaited>(func), std::forward<Completion>(completion)
        );
    }

    template <typename Exec, typename T>
    Var<T, std::exception_ptr> spawn_block(Exec& exec, Await<T> coro) noexcept
    {
        auto ready  = std::atomic<bool>{ false };
        auto result = Opt<Var<T, std::exception_ptr>>{};

        // NOTE: coro need to be wrapped since co_spawn on awaitable<T> requires T to be default constructible
        // read: https://github.com/chriskohlhoff/asio/issues/1303
        asio::co_spawn(
            exec,
            [&] -> Await<void> { result.emplace(co_await std::move(coro)); },
            [&](std::exception_ptr e) {
                if (e) {
                    result.emplace(e);
                }
                ready.store(true, std::memory_order::release);
                ready.notify_one();
            }
        );

        ready.wait(false);
        return std::move(result).value();
    }

    inline Errc to_generic_err(error_code ec, Errc fallback = Errc::io_error) noexcept
    {
        const auto& cat = ec.category();
#if MADBFS_NON_BOOST_ASIO
        if (cat == std::generic_category() or cat == std::system_category()) {
#else
        if (cat == boost::system::generic_category() or cat == asio::error::get_system_category()) {
#endif
            return static_cast<Errc>(ec.value());
        }
        return static_cast<bool>(fallback) ? fallback : Errc::invalid_argument;
    }

    template <typename Char, typename AStream>
    AExpect<usize, error_code> write_exact(AStream& stream, Span<const Char> in) noexcept
    {
        auto buf = asio::buffer(in);
        return asio::async_write(stream, buf, as_expected(asio::use_awaitable));
    }

    template <typename Char, typename AStream>
    AExpect<usize, error_code> read_exact(AStream& stream, Span<Char> out) noexcept
    {
        auto buf = asio::buffer(out.data(), out.size());
        return asio::async_read(stream, buf, as_expected(asio::use_awaitable));
    }

    using asio::buffer;
    using asio::dynamic_buffer;

    using asio::detached;
    using asio::use_awaitable;
    using asio::use_future;

    namespace this_coro = asio::this_coro;
    namespace operators = asio::experimental::awaitable_operators;

    namespace tcp
    {
        using Proto    = asio::ip::tcp;
        using Endpoint = Proto::endpoint;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    };

    namespace unix_socket
    {
        using Proto    = asio::local::stream_protocol;
        using Endpoint = Proto::endpoint;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    }

    namespace pipe
    {
        using Write = Token::as_default_on_t<asio::writable_pipe>;
        using Read  = Token::as_default_on_t<asio::readable_pipe>;
    }
}
