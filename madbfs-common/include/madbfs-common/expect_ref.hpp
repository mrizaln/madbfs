#pragma once
#include <concepts>
#include <expected>
#include <functional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace madbfs
{
    // ExpectRef<T, E> models "expected<T&, E>".
    //   - copy/assign/emplace REBIND (never assign through)
    //   - constness is shallow: const ExpectRef still yields T&
    //   - binding to rvalues is deleted
    //
    // I swear C++ committee are actively hostile to the C++ developers by omitting this very important
    // feature of std::expected.
    template <typename T, typename E = std::errc>
        requires std::is_object_v<T> and (not std::is_void_v<E>) and (not std::is_reference_v<E>)
    class ExpectRef;

    // traits
    // ------
    template <typename T>
    struct IsExpectedTrait : std::false_type
    {
    };

    template <typename T, typename E>
    struct IsExpectedTrait<std::expected<T, E>> : std::true_type
    {
        using Value = T;
        using Error = E;
    };

    template <typename T, typename E>
    struct IsExpectedTrait<ExpectRef<T, E>> : std::true_type
    {
        using Value = T;
        using Error = E;
    };

    template <typename T>
    concept IsExpect = IsExpectedTrait<T>::value;

    template <typename T, typename V>
    concept IsExpectOfVal = IsExpect<T> and std::same_as<typename IsExpectedTrait<T>::Value, V>;

    template <typename T, typename E>
    concept IsExpectOfErr = IsExpect<T> and std::same_as<typename IsExpectedTrait<T>::Error, E>;

    template <typename T, typename V, typename E>
    concept IsExpectOf = IsExpectOfVal<T, V> and IsExpectOfErr<T, E>;

}

namespace madbfs
{
    template <typename T, typename E>
        requires std::is_object_v<T> and (not std::is_void_v<E>) and (not std::is_reference_v<E>)
    class ExpectRef
    {
    public:
        using value_type      = T&;
        using error_type      = E;
        using unexpected_type = std::unexpected<E>;

        // ctor
        // ----
        ExpectRef(T& r) noexcept
            : m_value{ &r }
        {
        }
        ExpectRef(const T&&) = delete;

        template <typename... Args>
            requires std::constructible_from<E, Args...>
        explicit ExpectRef(std::unexpect_t, Args&&... args)
            : m_value{ std::unexpect, std::forward<Args>(args)... }
        {
        }

        ExpectRef(const std::unexpected<E>& u)
            : m_value{ u }
        {
        }

        ExpectRef(std::unexpected<E>&& u) noexcept
            : m_value{ std::move(u) }
        {
        }

        template <typename U>
            requires std::convertible_to<U*, T*>
        ExpectRef(const ExpectRef<U, E>& other)
            : m_value{
                other ? std::expected<T*, E>{ &*other }
                      : std::expected<T*, E>{ std::unexpect, other.error() },
            }
        {
        }

        // rebinding
        // ---------
        ExpectRef& operator=(const T&&) = delete;

        ExpectRef& operator=(T& r) noexcept
        {
            m_value = &r;
            return *this;
        }

        ExpectRef& operator=(const std::unexpected<E>& u)
        {
            m_value = u;
            return *this;
        }

        ExpectRef& operator=(std::unexpected<E>&& u)
        {
            m_value = std::move(u);
            return *this;
        }

        void emplace(const T&&) = delete;

        T& emplace(T& r) noexcept
        {
            m_value = &r;
            return r;
        }

        void swap(ExpectRef& o) noexcept(std::is_nothrow_swappable_v<std::expected<T*, E>>)
        {
            m_value.swap(o.m_value);
        }

        friend void swap(ExpectRef& a, ExpectRef& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

        // observers
        // ---------
        bool     has_value() const noexcept { return m_value.has_value(); }
        explicit operator bool() const noexcept { return m_value.has_value(); }

        T& operator*() const noexcept { return **m_value; }
        T* operator->() const noexcept { return *m_value; }

        T& value() const
        {
            if (not m_value) {
                throw std::bad_expected_access<E>{ m_value.error() };
            }
            return **m_value;
        }

        template <typename Self>
        auto&& error(this Self&& self) noexcept
        {
            return std::forward_like<Self>(self.m_value.error());
        }

        T& value_or(T& alt) const noexcept { return m_value ? **m_value : alt; }
        T& value_or(const T&&) const = delete;

        // monadic
        // -------
        template <typename Self, typename F>
            requires std::invocable<F, T&> and IsExpectOfErr<std::invoke_result_t<F, T&>, E>
        IsExpectOfErr<E> auto and_then(this Self&& self, F&& f)
        {
            using Res = std::invoke_result_t<F, T&>;

            if (self) {
                return std::invoke(std::forward<F>(f), *self);
            }

            return Res{ std::unexpect, std::forward_like<Self>(self.m_value.error()) };
        }

        template <typename Self, typename F>
            requires std::invocable<F, T&>
        IsExpectOfErr<E> auto transform(this Self&& self, F&& f)
        {
            using Val = std::invoke_result_t<F, T&>;
            using Res = std::expected<Val, E>;

            if (not self) {
                return Res{ std::unexpect, std::forward_like<Self>(self.m_value.error()) };
            }

            if constexpr (std::is_void_v<Val>) {
                std::invoke(std::forward<F>(f), *self);
                return Res{};
            } else {
                return Res{ std::invoke(std::forward<F>(f), *self) };
            }
        }

        template <typename Self, typename F>
            requires std::invocable<F, T&> and std::is_reference_v<std::invoke_result_t<F, T&>>
        IsExpectOfErr<E> auto transform(this Self&& self, F&& f)
        {
            using Val = std::invoke_result_t<F, T&>;
            using Res = ExpectRef<std::remove_reference_t<Val>, E>;

            if (not self) {
                return Res{ std::unexpect, std::forward_like<Self>(self.m_value.error()) };
            }

            return Res{ std::invoke(std::forward<F>(f), *self) };
        }

        std::expected<T*, E> underlying() const
            requires std::copy_constructible<E>
        {
            return m_value;
        }

        std::expected<std::reference_wrapper<T>, E> underlying_ref() const
            requires std::copy_constructible<E>
        {
            return m_value.transform([](T* t) { return std::ref(*t); });
        }

    private:
        std::expected<T*, E> m_value;
    };
}
