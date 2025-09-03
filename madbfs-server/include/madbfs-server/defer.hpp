#pragma once

#include <utility>

#ifdef DEFER
#    error "defer macro already defined"
#else

namespace madbfs::server::defer
{
    struct Tag
    {
    };

    template <typename F>
    struct Deferrer
    {
        F f;
        ~Deferrer() { f(); }
    };

    template <typename F>
    Deferrer<F> operator*(Tag, F&& f)
    {
        return { std::forward<F>(f) };
    }
}

#    define DEFER__DETAIL_CONCAT(PREFIX, NUM) PREFIX##NUM
#    define DEFER__DETAIL_VAR_NAME(NUM)       DEFER__DETAIL_CONCAT(zz_defer_number_, NUM)

#    define DEFER                                                                                            \
        auto DEFER__DETAIL_VAR_NAME(__COUNTER__) [[maybe_unused]]                                            \
        = madbfs::server::defer::Tag{}* [&] mutable noexcept -> void

#    define DEFER_C(...)                                                                                     \
        auto DEFER__DETAIL_VAR_NAME(__COUNTER__) [[maybe_unused]]                                            \
        = madbfs::server::defer::Tag{}* [__VA_ARGS__] mutable noexcept -> void

#endif    // defer
