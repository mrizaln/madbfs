#pragma once

/// custom completion token for Asio coroutines
/// inspiration:
/// https://gist.github.com/cstratopoulos/901b5cdd41d07c6ce6d83798b09ecf9b/863c1dbf3b063a5ff9ff2bdd834242ead556e74e

#ifndef MADBFS_NON_BOOST_ASIO
#    define MADBFS_NON_BOOST_ASIO 0
#endif

#if MADBFS_NON_BOOST_ASIO
#    include <asio.hpp>
#    include <asio/detail/handler_cont_helpers.hpp>
#    include <system_error>
#else
#    include <boost/asio.hpp>
#    include <boost/asio/detail/handler_cont_helpers.hpp>
#    include <boost/system/error_code.hpp>
#endif

#include <concepts>
#include <expected>
#include <type_traits>

namespace madbfs::async
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
            : token_{ static_cast<CompletionToken&&>(token) }
        {
        }

        // constructor
        template <typename T>
        constexpr explicit AsExpected(T&& completion_token)
            : token_{ static_cast<T&&>(completion_token) }
        {
        }

        // adapts an executor to add the AsExpected completion token as the default.
        template <typename InnerExecutor>
        struct ExecutorWithDefault : InnerExecutor
        {
            // NOTE: integration with Asio; don't change the name
            using default_completion_token_type = AsExpected;

            // Construct the adapted executor from the inner executor type.
            template <typename InnerExecutor1>
                requires (not std::same_as<InnerExecutor1, ExecutorWithDefault> //
                        and std::convertible_to<InnerExecutor1, InnerExecutor>)
            ExecutorWithDefault(const InnerExecutor1& ex) noexcept
                : InnerExecutor{ ex }
            {
            }
        };

        // Type alias to adapt an I/O object to use AsExpected as its default completion token type.
        // NOTE: integration with Asio. you can change the name, but it would be better if not
        template <typename T>
        using as_default_on_t =
            typename T::template rebind_executor<ExecutorWithDefault<typename T::executor_type>>::other;

        // function helper to adapt an I/O object to use AsExpected as its default completion token type.
        // NOTE: integration with Asio. you can change the name, but it would be better if not
        template <typename T>
        static as_default_on_t<std::decay_t<T>> as_default_on(T&& object)
        {
            return as_default_on_t<std::decay_t<T>>{ static_cast<T&&>(object) };
        }

        CompletionToken token_;
    };

    // Adapt a completion_token to specify that the completion handler
    // arguments should be combined into a single expected argument.
    template <typename CompletionToken>
    [[nodiscard]] inline constexpr AsExpected<std::decay_t<CompletionToken>> as_expected(
        CompletionToken&& completion_token
    )
    {
        return AsExpected<std::decay_t<CompletionToken>>{ static_cast<CompletionToken&&>(completion_token) };
    }
}

namespace madbfs::async::detail
{
    template <typename T, typename... Ts>
    concept AnyOf = (std::same_as<T, Ts> || ...);

#if MADBFS_NON_BOOST_ASIO
    using ErrorCode = std::error_code;
#else
    using ErrorCode = boost::system::error_code;
#endif

    // Class to adapt AsExpected as a completion handler
    template <typename Handler>
    struct AsExpectedHandler
    {
        // NOTE: integration with ASIO, don't change the name
        using result_type = void;

        template <typename Error>
            requires AnyOf<std::decay_t<Error>, ErrorCode, std::exception_ptr>
        void operator()(Error e)
        {
            using Expect = std::expected<void, ErrorCode>;
            if (e) {
                static_cast<Handler&&>(handler_)(std::unexpected{ e });
            } else {
                static_cast<Handler&&>(handler_)(Expect{});
            }
        }

        template <typename T, typename Error>
            requires AnyOf<std::decay_t<Error>, ErrorCode, std::exception_ptr>
        void operator()(Error e, T&& t)
        {
            using Expect = std::expected<std::decay_t<T>, ErrorCode>;
            if (e) {
                static_cast<Handler&&>(handler_)(std::unexpected{ e });
            } else {
                static_cast<Handler&&>(handler_)(Expect{ static_cast<T&&>(t) });
            }
        }

        Handler handler_;
    };

    template <typename Signature>
    struct AsExpectedSignatureTrait;

    template <typename Error>
        requires AnyOf<std::decay_t<Error>, ErrorCode, std::exception_ptr>
    struct AsExpectedSignatureTrait<void(Error)>
    {
        using type = void(std::expected<void, Error>);
    };

    template <typename T, typename Error>
        requires AnyOf<std::decay_t<Error>, ErrorCode, std::exception_ptr>
    struct AsExpectedSignatureTrait<void(Error, T)>
    {
        using type = void(std::expected<std::decay_t<T>, std::decay_t<Error>>);
    };
}

// NOTE: any partial specialization here are for integration with Asio, don't change the name of them. some
// typedef also for integration and have a fixed name

#if MADBFS_NON_BOOST_ASIO
namespace asio
#else
namespace boost::asio
#endif
{
    template <typename Sig>
    using AsExpectedSignature = madbfs::async::detail::AsExpectedSignatureTrait<Sig>;

    template <typename Handler>
    using AsExpectedHandler = madbfs::async::detail::AsExpectedHandler<Handler>;

    template <typename Handler>
    inline bool asio_handler_is_continuation(AsExpectedHandler<Handler>* this_handler)
    {
#if MADBFS_NON_BOOST_ASIO
        return asio_handler_cont_helpers::is_continuation(this_handler->handler_);
#else
        return boost_asio_handler_cont_helpers::is_continuation(this_handler->handler_);
#endif
    }

    template <typename CompletionToken, typename... Signatures>
    struct async_result<madbfs::async::AsExpected<CompletionToken>, Signatures...>
        : async_result<CompletionToken, typename AsExpectedSignature<Signatures>::type...>
    {
        template <typename Initiation>
        struct init_wrapper
        {
            init_wrapper(Initiation init)
                : initiation_{ static_cast<Initiation&&>(init) }
            {
            }

            template <typename Handler, typename... Args>
            void operator()(Handler&& handler, Args&&... args)
            {
                static_cast<Initiation&&>(initiation_)(
                    AsExpectedHandler<std::decay_t<Handler>>(static_cast<Handler&&>(handler)),
                    static_cast<Args&&>(args)...
                );
            }

            Initiation initiation_;
        };

        template <typename Initiation, typename RawCompletionToken, typename... Args>
        static auto initiate(Initiation&& initiation, RawCompletionToken&& token, Args&&... args)
            -> decltype(async_initiate<
                        conditional_t<
                            std::is_const_v<remove_reference_t<RawCompletionToken>>,
                            const CompletionToken,
                            CompletionToken>,
                        typename AsExpectedSignature<Signatures>::type...>(
                init_wrapper<std::decay_t<Initiation>>(static_cast<Initiation&&>(initiation)),
                token.token_,
                static_cast<Args&&>(args)...
            ))
        {
            return async_initiate<
                conditional_t<
                    is_const<remove_reference_t<RawCompletionToken>>::value,
                    const CompletionToken,
                    CompletionToken>,
                typename AsExpectedSignature<Signatures>::type...>(
                init_wrapper<decay_t<Initiation>>(static_cast<Initiation&&>(initiation)),
                token.token_,
                static_cast<Args&&>(args)...
            );
        }
    };

    template <template <typename, typename> class Associator, typename Handler, typename DefaultCandidate>
    struct associator<Associator, AsExpectedHandler<Handler>, DefaultCandidate>
        : Associator<Handler, DefaultCandidate>
    {
        static typename Associator<Handler, DefaultCandidate>::type get(const AsExpectedHandler<Handler>& h
        ) noexcept
        {
            return Associator<Handler, DefaultCandidate>::get(h.handler_);
        }

        static auto get(const AsExpectedHandler<Handler>& h, const DefaultCandidate& c) noexcept
            -> decltype(Associator<Handler, DefaultCandidate>::get(h.handler_, c))
        {
            return Associator<Handler, DefaultCandidate>::get(h.handler_, c);
        }
    };
}
