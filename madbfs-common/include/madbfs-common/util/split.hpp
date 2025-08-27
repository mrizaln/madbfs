#pragma once

#include "madbfs-common/aliases.hpp"

#include <algorithm>

namespace madbfs::util
{
    struct SplitDelim
    {
        using Variant = Var<char, Span<const char>>;

        constexpr SplitDelim(Str delim)
            : variant{ delim }
        {
        }

        constexpr SplitDelim(char delim)
            : variant{ delim }
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
            return std::visit(visitor, variant);
        }

        Variant variant;
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

            auto it = sr::find_if(m_str | sv::drop(m_idx), [this](char ch) { return m_delim.is_delim(ch); });
            if (it != m_str.end()) {
                auto pos = static_cast<usize>(it - m_str.begin());
                auto res = m_str.substr(m_idx, pos - m_idx);
                m_idx    = pos + 1;

                return res;
            }

            if (m_idx >= m_str.size()) {
                return std::nullopt;
            }

            auto res = m_str.substr(m_idx);
            m_idx    = m_str.size();    // mark end of line
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

        void  reset() noexcept { m_idx = 0; }
        bool  is_end() const noexcept { return m_idx >= m_str.size(); }
        usize offset() const noexcept { return m_idx; }

    private:
        Str        m_str;
        usize      m_idx   = 0;
        SplitDelim m_delim = SplitDelim{ ' ' };
    };

    template <usize N>
    struct SplitResult
    {
        Array<Str, N> result;
        Str           remainder;
    };

    inline Vec<Str> split(Str str, SplitDelim delim)
    {
        auto vec = Vec<Str>{};
        StringSplitter{ str, delim }.while_next([&](Str str) { vec.push_back(str); });
        return vec;
    }

    template <usize N>
    Opt<SplitResult<N>> split_n(Str str, SplitDelim delim)
    {
        auto splitter = StringSplitter{ str, delim };
        auto res      = Array<Str, N>{};

        auto idx = 0_usize;
        while (idx < N) {
            if (auto next = splitter.next()) {
                res[idx++] = *next;
            } else {
                break;
            }
        }

        if (idx < N) {
            return std::nullopt;
        }

        auto offset = splitter.offset();
        while (offset != str.size() and delim.is_delim(str[offset])) {
            ++offset;
        }

        return SplitResult<N>{ res, str.substr(offset) };
    }

    inline Str rstrip(Str str, SplitDelim delim = { " \t\n" })
    {
        while (not str.empty() and delim.is_delim(str.back())) {
            str.remove_suffix(1);
        }
        return str;
    }

    inline Str lstrip(Str str, SplitDelim delim = { " \t\n" })
    {
        while (not str.empty() and delim.is_delim(str.front())) {
            str.remove_prefix(1);
        }
        return str;
    }

    inline Str strip(Str str, SplitDelim delim = { " \t\n" })
    {
        return rstrip(lstrip(str, delim), delim);
    }
}
