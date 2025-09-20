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

namespace madbfs::net
{
#if not MADBFS_NON_BOOST_ASIO
    using namespace ::boost::asio;
    using error_code = boost::system::error_code;
#else
    using namespace ::asio;
    using error_code = std::error_code;
#endif
}

namespace madbfs
{
    template <typename T>
    using Await = net::awaitable<T>;

    template <typename T, typename E = Errc>
    using AExpect = Await<Expect<T, E>>;
}

namespace madbfs::inline concepts
{
    namespace detail
    {
        template <typename>
        struct AwaitableTraits : std::false_type
        {
        };

        template <typename T>
        struct AwaitableTraits<Await<T>> : std::true_type
        {
        };
    }

    template <typename T>
    concept IsAwaitable = concepts::detail::AwaitableTraits<T>::value;
}

namespace madbfs::async
{
    using Context   = net::io_context;
    using Token     = AsExpected<net::use_awaitable_t<>>;
    using Executor  = Context::executor_type;
    using WorkGuard = net::executor_work_guard<Executor>;

    using Timer = Token::as_default_on_t<net::steady_timer>;

    template <typename T>
    using Channel = Token::as_default_on_t<net::experimental::channel<void(net::error_code, T)>>;

    /**
     * @brief Spawn a new coroutine.
     *
     * @param exec Executor of the coroutine.
     * @param awaitable The coroutine.
     * @param completion Completion handler.
     *
     * @return Completion handler result.
     *
     * It is not advisable to use `async::detached` completion handler. Use other completion handler instead
     * like `async::use_future` or `async::use_awaitable`. If you want to detach the coroutine still, use
     * callback as the completion handler instead and handle the possible exception that may occur.
     */
    template <typename Exec, typename T, typename Compl>
    auto spawn(Exec& exec, Await<T>&& awaitable, Compl&& completion) noexcept
    {
        return net::co_spawn(exec, std::move(awaitable), std::forward<Compl>(completion));
    }

    /**
     * @brief Spawn then wait for coroutine to complete synchronously.
     *
     * @param exec Executor of the coroutine.
     * @param coro The coroutine in question.
     *
     * @return The result of the coroutine.
     *
     * This function will put the caller thread to sleep/wait state. Make sure the coroutine is run on an
     * already running async context or else this function will wait forever.
     */
    template <typename Exec, typename T>
    T block(Exec& exec, Await<T> coro) noexcept(false)
    {
        if constexpr (std::same_as<void, T>) {
            auto ready  = std::atomic<bool>{ false };
            auto except = std::exception_ptr{};

            net::co_spawn(exec, std::move(coro), [&](std::exception_ptr e) {
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

            net::co_spawn(exec, std::move(wrapped), [&](std::exception_ptr e) {
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

    /**
     * @brief Wait multiple coroutines asynchronously.
     *
     * @param awaitables Coroutines to be awaited.
     *
     * @return Combined result of the awaitables.
     */
    template <Range R>
        requires IsAwaitable<RangeValue<R>>
    Await<Vec<typename RangeValue<R>::value_type>> wait_all(R&& awaitables) noexcept(false)
    {
        namespace netx = net::experimental;

        auto exec  = co_await net::this_coro::executor;
        auto defer = [&](auto&& coro) { return async::spawn(exec, std::move(coro), net::deferred); };
        auto grp   = netx::make_parallel_group(awaitables | sv::transform(defer) | sr::to<std::vector>());

        auto [ord, e, res] = co_await grp.async_wait(netx::wait_for_all{}, net::use_awaitable);
        for (auto e : e) {
            if (e) {
                std::rethrow_exception(e);
            }
        }

        co_return res;
    }

    /**
     * @brief Wait multiple coroutines asynchronously.
     *
     * @param awaitables Coroutines to be awaited.
     *
     * @return Combined result of the awaitables.
     */
    template <typename... Ts>
    Await<Tup<ToUnit<Ts>...>> wait_all(Await<Ts>&&... awaitables) noexcept(false)
    {
        using net::experimental::awaitable_operators::operator&&;
        if constexpr ((std::same_as<Ts, void> and ...)) {
            co_await (std::move(awaitables) && ...);
            co_return Tup<ToUnit<Ts>...>{};
        } else {
            co_return co_await (std::move(awaitables) && ...);
        }
    }

    /**
     * @brief Spawn coroutine with timeout.
     *
     * @param awaitable Coroutine to be awaited.
     *
     * @return The result of the coroutine if completed within timeout, else `std::nullopt`;
     */
    template <typename T>
    Await<Opt<ToUnit<T>>> timeout(
        Await<T>&&            awaitable,
        Milliseconds          time,
        std::function<void()> on_timeout = {}
    )
    {
        using net::experimental::awaitable_operators::operator||;

        auto timer = Timer{ co_await net::this_coro::executor };

        timer.expires_after(time);
        auto res = co_await (std::move(awaitable) || timer.async_wait());

        switch (res.index()) {
        case 0: co_return Opt{ std::move(std::get<0>(res)) };
        case 1:
            if (on_timeout) {
                on_timeout();
            }
            co_return std::nullopt;
        }

        co_return std::nullopt;
    }

    /**
     * @brief Convert `std::error_code` to `Errc` (`std::errc`).
     *
     * @param ec The error code.
     * @param fallback Fallback error condition.
     *
     * @return Generic error condition if `std::error_code` is generic else `fallback`.
     */
    inline Errc to_generic_err(net::error_code ec, Errc fallback = Errc::io_error) noexcept
    {
        // conversion from boost error code to std error code
        auto err = static_cast<std::error_code>(ec);

        const auto& cat = err.category();
        if (cat == std::generic_category() or cat == std::system_category()) {
            return static_cast<Errc>(err.value());
        }

        return static_cast<bool>(fallback) ? fallback : Errc::invalid_argument;
    }

    /**
     * @brief Read exactly all data from stream fully into buffer.
     *
     * @param stream Asynchronous stream.
     * @param out Output buffer.
     *
     * @return The number of read buffer or error code.
     */
    template <typename Char, typename AStream>
    AExpect<usize, net::error_code> read_exact(AStream& stream, Span<Char> out) noexcept
    {
        auto buf = net::buffer(out.data(), out.size());
        return net::async_read(stream, buf, as_expected(net::use_awaitable));
    }

    /**
     * @brief Write exactly all data within buffer into stream.
     *
     * @param stream Asynchronous stream.
     * @param in Input buffer.
     *
     * @return The number of written buffer or error code.
     */
    template <typename Char, typename AStream>
    AExpect<usize, net::error_code> write_exact(AStream& stream, Span<const Char> in) noexcept
    {
        auto buf = net::buffer(in);
        return net::async_write(stream, buf, as_expected(net::use_awaitable));
    }

    /**
     * @brief Discard data from stream.
     *
     * @param stream Asynchronous stream.
     * @param size Number of data to be discarded.
     *
     * @return None or error code.
     */
    template <typename AStream>
    AExpect<void, net::error_code> discard(AStream& stream, usize size) noexcept
    {
        auto sink = Array<char, 1024>{};
        while (size != 0) {
            auto buf = Span{ sink.data(), std::min(sink.size(), size) };
            if (auto n = co_await read_exact(stream, buf); not n) {
                co_return Unexpect{ n.error() };
            } else {
                size -= *n;
            }
        }

        co_return Expect<void, net::error_code>{};
    }

    /**
     * @brief Read data from stream using LV encoding into buffer.
     *
     * @param stream Asynchronous stream.
     * @param out Output buffer.
     *
     * @return The number of data written into `buffer`
     *
     * This function will check the buffer size and if the remote sends an LV encoded data with the size
     * exceeding this buffer, error condition `EMSGSIZE` will be returned and the data will be discarded.
     */
    template <typename Char, typename AStream>
    AExpect<usize, net::error_code> read_lv(AStream& stream, Span<Char> out) noexcept
    {
        using LenBuf = Array<char, 4>;

        auto len_buf = LenBuf{};
        if (auto n = co_await read_exact<char>(stream, len_buf); not n) {
            co_return Unexpect{ n.error() };
        }

        auto len = ::ntohl(std::bit_cast<u32>(len_buf));
        if (len > out.size()) {
            auto res = co_await discard(stream, len);
            co_return Unexpect{ res.error_or(std::make_error_code(Errc::message_size)) };
        }

        co_return co_await read_exact<Char>(stream, { out.data(), len });
    }

    /**
     * @brief Read data from stream using LV encoding into buffer
     *
     * @param stream Asynchronous stream.
     * @param out Output buffer.
     * @param max Max number of data to be written to buffer.
     *
     * @return The number of data written to buffer.
     *
     * This function is an overload that takes a resizable buffer (`std::basic_string` and `std::vector`) as
     * its buffer to allow for more flexible read. The `max` parameter limits the number data written to
     * the buffer. You may set this to the maximum number possible that can be represented by `std::size_t` to
     * effectively disable the limitation.
     */
    template <typename Char, typename AStream, template <typename... Ts> typename Tmpl>
        requires std::same_as<Tmpl<Char>, std::vector<Char>>
              or std::same_as<Tmpl<Char>, std::basic_string<Char>>
    AExpect<usize, net::error_code> read_lv(AStream& stream, Tmpl<Char>& out, usize max) noexcept
    {
        using LenBuf = Array<char, 4>;

        auto len_buf = LenBuf{};
        if (auto n = co_await read_exact<char>(stream, len_buf); not n) {
            co_return Unexpect{ n.error() };
        }

        auto len = ::ntohl(std::bit_cast<u32>(len_buf));
        if (len > max) {
            auto res = co_await discard(stream, len);
            co_return Unexpect{ res.error_or(std::make_error_code(Errc::message_size)) };
        } else if (len > out.size()) {
            out.resize(len);
        }

        co_return co_await read_exact<Char>(stream, { out.data(), len });
    }

    /**
     * @brief Write data to stream using LV encoding from buffer.
     *
     * @param stream Asynchronous stream.
     * @param out Output buffer.
     *
     * @return The number of data written into `buffer`.
     */
    template <typename Char, typename AStream>
    AExpect<usize, net::error_code> write_lv(AStream& stream, Span<const Char> in) noexcept
    {
        using LenBuf = Array<char, 4>;

        auto len = std::bit_cast<LenBuf>(::htonl(static_cast<u32>(in.size())));
        if (auto n = co_await write_exact<char>(stream, len); not n) {
            co_return Unexpect{ n.error() };
        }

        co_return co_await write_exact<char>(stream, in);
    }

    /**
     * @brief Get current executor.
     */
    inline net::this_coro::executor_t current_executor() noexcept
    {
        return net::this_coro::executor;
    }

    using net::detached;
    using net::use_awaitable;
    using net::use_future;

    namespace tcp
    {
        using Proto    = net::ip::tcp;
        using Endpoint = Proto::endpoint;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    };

    namespace unix_socket
    {
        using Proto    = net::local::stream_protocol;
        using Endpoint = Proto::endpoint;
        using Acceptor = Token::as_default_on_t<Proto::acceptor>;
        using Socket   = Token::as_default_on_t<Proto::socket>;
    }

    namespace pipe
    {
        using Write = Token::as_default_on_t<net::writable_pipe>;
        using Read  = Token::as_default_on_t<net::readable_pipe>;
    }
}
