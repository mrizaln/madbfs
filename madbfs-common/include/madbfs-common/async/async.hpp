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
#    include <asio/experimental/channel.hpp>
#    include <asio/experimental/parallel_group.hpp>
#else
#    include <boost/asio.hpp>
#    include <boost/asio/error.hpp>
#    include <boost/asio/experimental/awaitable_operators.hpp>
#    include <boost/asio/experimental/channel.hpp>
#    include <boost/asio/experimental/parallel_group.hpp>
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
}

namespace madbfs::async
{
    using Context   = asio::io_context;
    using Token     = AsExpected<asio::use_awaitable_t<>>;
    using Executor  = Context::executor_type;
    using WorkGuard = asio::executor_work_guard<Executor>;

    using Timer = Token::as_default_on_t<asio::steady_timer>;

    template <typename T>
    using Channel = Token::as_default_on_t<asio::experimental::channel<void(error_code, T)>>;

    template <typename Exec, typename Awaited, typename Completion>
    auto spawn(Exec&& ex, Awaited&& func, Completion&& completion) noexcept
    {
        return asio::co_spawn(
            std::forward<Exec>(ex), std::forward<Awaited>(func), std::forward<Completion>(completion)
        );
    }

    template <typename Exec, typename T>
    T spawn_block(Exec& exec, Await<T> coro) noexcept(false)
    {
        if constexpr (std::same_as<void, T>) {
            auto ready  = std::atomic<bool>{ false };
            auto except = std::exception_ptr{};

            asio::co_spawn(exec, std::move(coro), [&](std::exception_ptr e) {
                except = e;
                ready.store(true, std::memory_order::release);
                ready.notify_one();
            });

            ready.wait(false, std::memory_order::acquire);
            if (except) {
                std::rethrow_exception(except);
            }
        } else {
            // NOTE: coro needs to be wrapped into a coro that returns void since co_spawn on awaitable<T>
            // requires T to be default constructible [https://github.com/chriskohlhoff/asio/issues/1303]

            auto ready   = std::atomic<bool>{ false };
            auto except  = std::exception_ptr{};
            auto result  = Opt<T>{};
            auto wrapped = [&] -> Await<void> { result.emplace(co_await std::move(coro)); };

            asio::co_spawn(exec, std::move(wrapped), [&](std::exception_ptr e) {
                except = e;
                ready.store(true, std::memory_order::release);
                ready.notify_one();
            });

            ready.wait(false, std::memory_order::acquire);
            if (except) {
                std::rethrow_exception(except);
            }

            return std::move(result).value();
        }
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

    inline asio::this_coro::executor_t current_executor() noexcept
    {
        return asio::this_coro::executor;
    }

    using asio::buffer;
    using asio::dynamic_buffer;

    using asio::detached;
    using asio::use_awaitable;
    using asio::use_future;

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

    namespace operators
    {
        using namespace asio::experimental::awaitable_operators;
    }
}
