#pragma once

#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <variant>

namespace adbfs
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

        using Unit = std::monostate;
        using Str  = std::string_view;

        template <typename T>
        using Opt = std::optional<T>;

        template <typename... T>
        using Var = std::variant<T...>;

        template <typename T>
        using Span = std::span<T>;

        namespace sv = std::views;
        namespace sr = std::ranges;
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
}
