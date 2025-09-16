#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <variant>
#include <vector>
#include <version>

#ifdef __cpp_lib_generator
#    include <generator>
#endif

namespace madbfs
{
    inline namespace aliases
    {
        using i8  = std::int8_t;
        using i16 = std::int16_t;
        using i32 = std::int32_t;
        using i64 = std::int64_t;

        using u8  = std::uint8_t;
        using u16 = std::uint16_t;
        using u32 = std::uint32_t;
        using u64 = std::uint64_t;

        using usize = std::size_t;
        using isize = std::ptrdiff_t;

        using f32 = float;
        using f64 = double;

        using Unit   = std::monostate;
        using Str    = std::string_view;
        using String = std::string;

        template <typename T>
        using Init = std::initializer_list<T>;

        template <typename T, typename U>
        using Pair = std::pair<T, U>;

        template <typename... Ts>
        using Tup = std::tuple<Ts...>;

        template <typename T>
        using Opt = std::optional<T>;

        template <typename... T>
        using Var = std::variant<T...>;

        template <typename T>
        using Span = std::span<T>;

        template <typename T, std::size_t N>
        using Array = std::array<T, N>;

        template <typename T>
        using Vec = std::vector<T>;

        template <typename T>
        using Uniq = std::unique_ptr<T>;

        template <typename T>
        using Shared = std::shared_ptr<T>;

        template <typename T, typename E = std::errc>
        using Expect = std::expected<T, E>;

        template <typename E = std::errc>
        using Unexpect = std::unexpected<E>;

        using SystemClock = std::chrono::system_clock;
        using SteadyClock = std::chrono::steady_clock;

        using Seconds      = std::chrono::seconds;
        using Milliseconds = std::chrono::milliseconds;
        using Nanoseconds  = std::chrono::nanoseconds;

#ifdef __cpp_lib_generator
        template <typename T>
        using Gen = std::generator<T>;
#endif

        using Errc = std::errc;

        template <typename T>
        using Ref = std::reference_wrapper<T>;

        namespace sr = std::ranges;
        namespace sv = std::views;
    }

    inline namespace concepts
    {
        template <typename T>
        concept Range = std::ranges::range<T>;

        template <typename T>
        concept VRange = std::ranges::viewable_range<T>;

        template <typename T>
        concept FRange = std::ranges::forward_range<T>;

        template <Range T>
        using RangeValue = std::ranges::range_value_t<T>;

        template <typename Fn, typename... Args>
        concept Invocable = std::invocable<Fn, Args...>;

        template <typename Fn, typename... Args>
        using InvokeResult = std::invoke_result_t<Fn, Args...>;
    }

    inline namespace util_functions
    {
        inline constexpr auto sink_void = [](auto&&) { };
        inline constexpr auto sink_unit = [](auto&&) { return Unit{}; };

        /**
         * @brief Convert `std::optional` to `std::expected`.
         *
         * @param opt Opt to be converted.
         * @param err Err value when `opt` is `std::nullopt`;
         *
         * @return New `std::expected` with the value from `opt` or `err`.
         */
        template <typename T>
        Expect<T> ok_or(Opt<T>&& opt, std::errc err) noexcept
        {
            if (opt.has_value()) {
                return std::move(opt).value();
            } else {
                return Unexpect{ err };
            }
        }

        /**
         * @brief Simple project member function to a type and return as lambda.
         *
         * @param args Member function to be called on.
         * @param args The args to be passed on to the member function.
         *
         * @return The Resulting lambda.
         *
         * This function will convert references into `std::reference_wrapper`
         */
        template <typename T, typename Ret, typename... Args>
        constexpr auto proj(Ret (T::*fn)(Args...) const, std::type_identity_t<Args>... args) noexcept
        {
            if constexpr (std::is_reference_v<Ret>) {
                return [fn, ... args = std::forward<Args>(args)](const T& t) mutable {
                    return std::cref((t.*fn)(std::forward<Args>(args)...));
                };
            } else {
                return [fn, ... args = std::forward<Args>(args)](const T& t) mutable -> decltype(auto) {
                    return (t.*fn)(std::forward<Args>(args)...);
                };
            }
        }

        /**
         * @brief Simple project member function to a type and return as lambda.
         *
         * @param args Member function to be called on.
         * @param args The args to be passed on to the member function.
         *
         * @return The Resulting lambda.
         *
         * This function will convert references into `std::reference_wrapper`
         */
        template <typename T, typename Ret, typename... Args>
        constexpr auto proj(Ret (T::*fn)(Args...), std::type_identity_t<Args>... args) noexcept
        {
            if constexpr (std::is_reference_v<Ret>) {
                return [fn, ... args = std::forward<Args>(args)](T& t) mutable {
                    return std::cref((t.*fn)(std::forward<Args>(args)...));
                };
            } else {
                return [fn, ... args = std::forward<Args>(args)](T& t) mutable -> decltype(auto) {
                    return (t.*fn)(std::forward<Args>(args)...);
                };
            }
        }

        template <typename T, typename C>
        constexpr auto proj(T C::* mem) noexcept
        {
            return [mem]<typename CC>(CC&& c) { return std::forward<CC>(c).*mem; };
        }
    }

    inline namespace literals
    {
        // clang-format off
        constexpr aliases::i8  operator""_i8 (unsigned long long value) { return static_cast<aliases::i8 >(value); }
        constexpr aliases::i16 operator""_i16(unsigned long long value) { return static_cast<aliases::i16>(value); }
        constexpr aliases::i32 operator""_i32(unsigned long long value) { return static_cast<aliases::i32>(value); }
        constexpr aliases::i64 operator""_i64(unsigned long long value) { return static_cast<aliases::i64>(value); }

        constexpr aliases::u8  operator""_u8 (unsigned long long value) { return static_cast<aliases::u8 >(value); }
        constexpr aliases::u16 operator""_u16(unsigned long long value) { return static_cast<aliases::u16>(value); }
        constexpr aliases::u32 operator""_u32(unsigned long long value) { return static_cast<aliases::u32>(value); }
        constexpr aliases::u64 operator""_u64(unsigned long long value) { return static_cast<aliases::u64>(value); }

        constexpr aliases::usize operator""_usize(unsigned long long value) { return static_cast<aliases::usize>(value); }
        constexpr aliases::isize operator""_isize(unsigned long long value) { return static_cast<aliases::isize>(value); }

        constexpr aliases::f32 operator""_f32(long double value) { return static_cast<aliases::f32>(value); }
        constexpr aliases::f64 operator""_f64(long double value) { return static_cast<aliases::f64>(value); }
        // clang-format on
    }

    inline namespace meta
    {
        template <typename T>
        struct ToUnitTrait
        {
            using Type = T;
        };

        template <>
        struct ToUnitTrait<void>
        {
            using Type = Unit;
        };

        template <typename T>
        using ToUnit = ToUnitTrait<T>::Type;
    }
}

#ifndef MADBFS_RAPIDHASH_ENABLED
#    define MADBFS_RAPIDHASH_ENABLED 0
#endif

#if MADBFS_RAPIDHASH_ENABLED
#    include <rapidhash.h>

// enable hashing for any type that has unique object representation
template <typename T>
    requires std::has_unique_object_representations<T>::value
struct std::hash<T>
{
    std::size_t operator()(const T& value) const noexcept { return rapidhash(&value, sizeof(T)); }
};
#endif
