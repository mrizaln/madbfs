#pragma once

#include "common.hpp"    // for type aliases and namespace aliases (sr & sv)

#include <algorithm>
#include <variant>

namespace adbfs::util
{
    struct SplitDelim
    {
        using Variant = Var<char, Span<const char>>;

        constexpr SplitDelim(Variant delim)
            : m_variant{ delim }
        {
        }

        constexpr bool is_delim(char ch) const
        {
            auto visitor = [&](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<T, char>) {
                    return ch == value;
                } else {
                    return sr::find(value, ch) != value.end();
                }
            };
            return std::visit(visitor, m_variant);
        }

        Variant m_variant;
    };

    class StringSplitter
    {
    public:
        StringSplitter(Str str, SplitDelim delim) noexcept
            : m_str{ str }
            , m_delim{ delim }
        {
        }

        Opt<Str> next() noexcept
        {
            if (is_end()) {
                return std::nullopt;
            }

            while (m_idx < m_str.size() and m_delim.is_delim(m_str[m_idx])) {
                ++m_idx;
            }

            auto it = std::ranges::find_if(m_str | std::views::drop(m_idx), [this](char ch) {
                return m_delim.is_delim(ch);
            });

            if (it == m_str.end()) {
                auto res = m_str.substr(m_idx);
                m_idx    = m_str.size();    // mark end of line
                return res;
            }

            auto pos = static_cast<std::size_t>(it - m_str.begin());
            auto res = m_str.substr(m_idx, pos - m_idx);
            m_idx    = pos + 1;

            return res;
        }

        template <std::invocable<Str> Fn>
        usize while_next(Fn&& fn) noexcept
        {
            auto count = 0_usize;
            while (auto res = next()) {
                fn(*res);
                ++count;
            }
            return count;
        }

        void reset() noexcept { m_idx = 0; }
        bool is_end() const noexcept { return m_idx >= m_str.size(); }

    private:
        Str         m_str;
        std::size_t m_idx   = 0;
        SplitDelim  m_delim = SplitDelim{ ' ' };
    };
}
