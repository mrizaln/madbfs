#pragma once

#include <concepts>
#include <utility>

namespace madbfs::util
{
    template <typename Fn>
        requires std::move_constructible<Fn> and std::invocable<Fn>
    class DeferGuard
    {
    public:
        DeferGuard()                              = delete;
        DeferGuard(const DeferGuard&)             = delete;
        DeferGuard& operator=(const DeferGuard&)  = delete;
        DeferGuard& operator=(DeferGuard&& other) = delete;

        DeferGuard(Fn fn)
            : m_fn{ std::move(fn) }
        {
        }

        DeferGuard(DeferGuard&& other)
            : m_fn{ std::move(other.m_fn) }
        {
        }

        ~DeferGuard() { std::move(m_fn)(); }

    private:
        Fn m_fn;
    };

    template <typename>
    struct DeferredTraits : std::false_type
    {
    };

    template <typename Fn>
    struct DeferredTraits<DeferGuard<Fn>> : std::true_type
    {
    };

    template <typename Fn>
    concept Deferred = DeferredTraits<Fn>::value;

    template <typename Fn>
        requires std::move_constructible<Fn> and std::invocable<Fn>
    util::Deferred auto defer(Fn fn)
    {
        return util::DeferGuard{ std::move(fn) };
    }
}
