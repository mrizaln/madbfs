#pragma once

#include "adbfsm/common.hpp"

#include <cassert>
#include <generator>
#include <print>

namespace adbfsm::path
{
    /**
     * @class Path
     * @brief Represent a file path in Linux system.
     *
     * This class is a simple wrapper over the underlying path string. It is like Str where it doesn't
     * allocate anything.
     */
    class Path
    {
    public:
        friend constexpr Opt<Path> create(Str path);

        constexpr Path() = default;

        constexpr bool is_root() const { return m_dirname == "/" and m_basename == "/"; }

        constexpr Str filename() const { return m_basename; }
        constexpr Str parent() const { return m_dirname; }
        constexpr Str fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        /**
         * @brief Creates generator that iterates over the path.
         */
        std::generator<Str> iter() const;

        /**
         * @brief Creates generator that iterates over the path up till the parent directory.
         */
        std::generator<Str> iter_parent() const;

    private:
        constexpr Path(Str dirname, Str name)
            : m_dirname{ dirname }
            , m_basename{ name }
        {
        }

        Str m_dirname;
        Str m_basename;
    };

    /**
     * @brief Create a Path from a string.
     *
     * If the path pased into this function is not absolute or empty, it will return `std::nullopt`.
     * Repeating '/' on the leading and trailing edge are ignored. If the repeating '/' is on the middle
     * however, it will be preserved.
     */
    constexpr Opt<Path> create(Str path)
    {
        if (path.empty() or path.front() != '/') {
            return std::nullopt;
        }

        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
        }
        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }

        if (path == "/") {
            return Path{ "/", "/" };
        }

        auto prev    = 1uz;
        auto current = 1uz;

        while (current < path.size()) {
            while (current < path.size() and path[current] == '/') {
                ++current;
            }

            // current = path.find('/', current);    // can't compile in gcc somehow??
            // if (current == Str::npos) {
            //     break;
            // }
            // prev = current;

            auto it = sr::find(path | sv::drop(current), '/');
            if (it == path.end()) {
                current = path.size();
                break;
            }
            current = static_cast<std::size_t>(it - path.begin());
            prev    = current;
        }

        const auto dirname_end = prev;

        // in case the basename contains repeated '//' like in the case '/home/user/documents/////note.md'
        auto basename_start = prev;
        while (path[basename_start] == '/') {
            ++basename_start;
        }

        return Path{ path.substr(0, dirname_end), path.substr(basename_start) };
    }

    /**
     * @brief Creates a generator that iterates over the path in the given string.
     *
     * @param path The path to iterate over.
     *
     * This function is here to give a way to opt-out of needing to create `Path` first before iterating
     * over path string.
     */
    Opt<std::generator<Str>> iter_str(Str path);
}

namespace adbfsm::path::inline literals
{
    namespace detail
    {
        template <std::size_t N>
        struct FixedStr
        {
            char m_data[N]{};
            constexpr FixedStr() = default;
            constexpr FixedStr(const char (&str)[N]) { std::ranges::copy_n(str, N, m_data); }
            constexpr Str str() const { return { m_data, N - 1 }; }
        };
    }

    template <detail::FixedStr Str>
    consteval Path operator""_path()
    {
        if (auto path = create(Str.str()); path.has_value()) {
            return *path;
        }
        throw std::invalid_argument{ "Invalid path" };
    }
}
