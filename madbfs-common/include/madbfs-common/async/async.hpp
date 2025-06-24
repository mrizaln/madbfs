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

    template <typename T>
    using AExpect = Await<Expect<T>>;

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
    auto spawn(Exec&& ex, Awaited&& func, Completion&& completion)
    {
        return asio::co_spawn(
            std::forward<Exec>(ex), std::forward<Awaited>(func), std::forward<Completion>(completion)
        );
    }

    template <typename Exec, typename T>
    Var<T, std::exception_ptr> spawn_block(Exec& exec, Await<T> coro)
    {
        auto ready  = std::atomic<bool>{ false };
        auto result = Opt<Var<T, std::exception_ptr>>{};

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

    inline Errc to_generic_err(error_code ec, Errc fallback = Errc::io_error)
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
    Await<Pair<error_code, usize>> write_exact(AStream& stream, Span<const Char> in)
    {
        auto written = 0uz;
        while (written < in.size()) {
            auto buffer = asio::buffer(in.data() + written, in.size() - written);
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

    template <typename Char, typename AStream>
    Await<Pair<error_code, usize>> read_exact(AStream& stream, Span<Char> out)
    {
        auto read = 0uz;
        while (read < out.size()) {
            auto buffer = asio::buffer(out.data() + read, out.size() - read);
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
