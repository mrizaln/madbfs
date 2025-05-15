#pragma once

/// custom completion token for Asio coroutines
/// inspiration:
/// https://gist.github.com/cstratopoulos/901b5cdd41d07c6ce6d83798b09ecf9b/863c1dbf3b063a5ff9ff2bdd834242ead556e74e

#include <boost/asio.hpp>
#include <boost/asio/detail/handler_alloc_helpers.hpp>

#include <concepts>
#include <expected>
#include <system_error>
#include <type_traits>
#include <utility>

namespace adbfsm::async
{
    template <typename CompletionToken>
    struct AsExpected
    {
        struct DefaultConstructorTag
        {
        };

        // default constructor
        /**
         * this constructor is only valid if the underlying completion token is default constructible and move
         * constructible. the underlying completion token is itself defaulted as an argument to allow it to
         * capture a source location.
         */
        constexpr AsExpected(
            DefaultConstructorTag = DefaultConstructorTag{},
            CompletionToken token = CompletionToken{}    //
        )
            : m_token(std::move(token))
        {
        }

        // constructor
        template <typename T>
        constexpr explicit AsExpected(T&& completion_token)
            : m_token(std::forward<T>(completion_token))
        {
        }

        // adapts an executor to add the as_expected_t completion token as the default.
        template <typename InnerExecutor>
        struct ExecutorWithDefault : InnerExecutor
        {
            // NOTE: integration with Asio; keep don't change the name
            using default_completion_token_type = AsExpected;

            // Construct the adapted executor from the inner executor type.
            template <typename InnerExecutor1>
                requires (!std::same_as<InnerExecutor1, ExecutorWithDefault> && std::convertible_to<InnerExecutor1, InnerExecutor>)
            ExecutorWithDefault(InnerExecutor1&& ex) noexcept
                : InnerExecutor(std::forward<InnerExecutor1>(ex))
            {
            }
        };

        // Type alias to adapt an I/O object to use as_expected_t as its default completion token type.
        // NOTE: integration with Asio. you can change the name, but it would be better if not
        template <typename T>
        using as_default_on_t
            = T::template rebind_executor<ExecutorWithDefault<typename T::executor_type>>::other;

        // function helper to adapt an I/O object to use as_expected_t as its default completion token type.
        // NOTE: integration with Asio. you can change the name, but it would be better if not
        template <typename T>
        static std::decay_t<T>::template rebind_executor<
            ExecutorWithDefault<typename std::decay_t<T>::executor_type>>::other
        as_default_on(T&& object)
        {
            return std::decay_t<T>::template rebind_executor<
                ExecutorWithDefault<typename std::decay_t<T>::executor_type>>::other(std::forward<T>(object));
        }
        CompletionToken m_token;
    };

    namespace detail
    {

        template <typename T, typename... Ts>
        concept AnyOf = (std::same_as<T, Ts> || ...);

        template <typename T>
        concept Error = AnyOf<T, std::error_code, const std::error_code&, std::exception_ptr>;

        // Class to adapt as_expected_t as a completion handler
        template <typename Handler>
        struct ExpectedHandler
        {
            template <Error E>
            void operator()(E e)
            {
                using Result = std::expected<void, std::remove_cvref_t<E>>;

                if (e) {
                    m_handler(Result{ std::unexpected(e) });
                } else {
                    m_handler(Result{});
                }
            }

            template <typename T, Error E>
            void operator()(E e, T t)
            {
                using Result = std::expected<T, std::remove_cvref_t<E>>;

                if (e) {
                    m_handler(Result{ std::unexpected(e) });
                } else {
                    m_handler(Result{ std::move(t) });
                }
            }

            Handler m_handler;
        };

        template <typename Signature>
        struct ExpectedSignatureTrait;

        template <Error E>
        struct ExpectedSignatureTrait<void(E)>
        {
            using type = void(std::expected<void, E>);
        };

        template <typename T, Error E>
        struct ExpectedSignatureTrait<void(E, T)>
        {
            using type = void(std::expected<T, E>);
        };

        template <typename Signature>
        using ExpectedSignatureTrait_t = ExpectedSignatureTrait<Signature>::type;

    }    // namespace detail

}    // namespace async

// NOTE: any partial specialization here are for integration with Asio, don't change the name of it. some
// typedef also for integration and have a fixed name
namespace boost::asio
{

    template <typename T>
    using ExpectedHandler = adbfsm::async::detail::ExpectedHandler<T>;

    template <typename CompletionToken, typename Signature>
    class async_result<adbfsm::async::AsExpected<CompletionToken>, Signature>
    {
    private:
        using ExpectedSignature = adbfsm::async::detail::ExpectedSignatureTrait_t<Signature>;
        using ReturnType        = async_result<CompletionToken, ExpectedSignature>::return_type;

    public:
        template <typename Initiation, typename... Args>
        static ReturnType initiate(
            Initiation&&                                 initiation,
            adbfsm::async::AsExpected<CompletionToken>&& token,
            Args&&... args
        )
        {
            return async_initiate<CompletionToken, ExpectedSignature>(
                [init = std::forward<Initiation>(initiation)]<typename Handler, typename... CArgs>(
                    Handler&& handler, CArgs&&... callArgs    //
                ) mutable {
                    std::move(init)(
                        ExpectedHandler<Handler>{ std::forward<Handler>(handler) },
                        std::forward<CArgs>(callArgs)...    //
                    );
                },
                token.m_token,
                std::forward<Args>(args)...
            );
        }
    };

    template <typename Handler, typename Executor>
    struct associated_executor<ExpectedHandler<Handler>, Executor>
    {
        using type = associated_executor<Handler, Executor>::type;

        static type get(const ExpectedHandler<Handler>& h, const Executor& ex = Executor()) noexcept
        {
            return associated_executor<Handler, Executor>::get(h.m_handler, ex);
        }
    };

    template <typename Handler, typename Allocator>
    struct associated_allocator<ExpectedHandler<Handler>, Allocator>
    {
        using type = associated_allocator<Handler, Allocator>::type;

        static type get(const ExpectedHandler<Handler>& h, const Allocator& a = Allocator()) noexcept
        {
            return associated_allocator<Handler, Allocator>::get(h.m_handler, a);
        }
    };
}    // namespace asio
