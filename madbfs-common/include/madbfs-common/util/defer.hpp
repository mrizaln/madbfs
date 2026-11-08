#pragma once

#include <concepts>
#include <utility>

namespace madbfs::util
{
    /**
     * @class DeferGuard
     *
     * @brief RAII-based defer functionality.
     */
    template <typename Fn>
        requires std::move_constructible<Fn> and std::invocable<Fn>
    class [[nodiscard]] DeferGuard
    {
    public:
        DeferGuard()                              = delete;
        DeferGuard(const DeferGuard&)             = delete;
        DeferGuard& operator=(const DeferGuard&)  = delete;
        DeferGuard& operator=(DeferGuard&& other) = delete;

        DeferGuard(Fn fn)
            : m_fn{ std::move(fn) }
            , m_engaged{ true }
        {
        }

        DeferGuard(DeferGuard&& other)
            : m_fn{ std::move(other.m_fn) }
            , m_engaged{ std::exchange(other.m_engaged, false) }
        {
        }

        ~DeferGuard()
        {
            if (m_engaged) {
                std::move(m_fn)();
                m_engaged = false;
            }
        }

    private:
        Fn   m_fn;
        bool m_engaged;
    };

    template <typename>
    struct DeferredTraits : std::false_type
    {
    };

    template <typename Fn>
    struct DeferredTraits<DeferGuard<Fn>> : std::true_type
    {
    };

    /**
     * @brief Deferred concept.
     *
     * @tparam Fn Deferred action.
     *
     * This concept is useful in the context where you need to specify return type but you can't name the
     * function being deferred, like lambda.
     */
    template <typename Fn>
    concept Deferred = DeferredTraits<Fn>::value;

    /**
     * @brief Create a deferred action.
     *
     * @param fn Deferred action.
     *
     * @return A defer guard.
     *
     * The returned guard must not be ignored else the action will run immediately instead of done at the end
     * of scope.
     */
    template <typename Fn>
        requires std::move_constructible<Fn> and std::invocable<Fn>
    DeferGuard<Fn> defer(Fn fn)
    {
        return { std::move(fn) };
    }
}
