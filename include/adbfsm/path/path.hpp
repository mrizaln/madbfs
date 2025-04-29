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
        friend class PathBuf;
        friend constexpr Opt<Path> create(Str path);

        constexpr Path() = default;

        constexpr bool is_root() const { return m_dirname == "/" and m_basename == "/"; }
        constexpr bool is_dir() const { return m_is_dir; }

        constexpr Str filename() const { return m_basename; }
        constexpr Str parent() const { return m_dirname; }
        constexpr Str fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        constexpr Path parent_path() const
        {
            if (m_dirname == "/") {
                return { "/", "/", true };
            }

            auto base_start = m_dirname.size();
            while (m_dirname[--base_start] != '/') { }

            auto dir_end = base_start;
            while (dir_end > 0 and m_dirname[dir_end] == '/') {
                --dir_end;
            }

            auto begin = m_dirname.begin();
            auto end   = m_dirname.end();

            if (dir_end == 0) {
                return { { begin, begin + base_start + 1 }, { begin + base_start + 1, end }, true };
            }

            return { { begin, begin + dir_end + 1 }, { begin + base_start + 1, end }, true };
        }

        /**
         * @brief Creates generator that iterates over the path.
         */
        std::generator<Str> iter() const;

    private:
        constexpr Path(Str dirname, Str name, bool is_dir)
            : m_dirname{ dirname }
            , m_basename{ name }
            , m_is_dir{ is_dir }
        {
        }

        Str  m_dirname;
        Str  m_basename;
        bool m_is_dir = false;
    };

    /**
     * @class PathBuf
     * @brief Represent a file path in Linux system that owns its path buffer.
     */
    class PathBuf
    {
    public:
        PathBuf(Path path)
            : m_buf{ path.fullpath() }
            , m_path{
                { m_buf.data(), path.parent().size() },
                { m_buf.data() + (path.filename().data() - path.fullpath().data()), path.filename().size() },
                path.is_dir(),
            }
        {
        }

        Path as_path() const { return m_path; }

    private:
        String m_buf;
        Path   m_path;
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

        auto is_dir = false;
        while (path.size() > 1 and path.back() == '/') {
            is_dir = true;
            path.remove_suffix(1);
        }
        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
        }

        if (path == "/") {
            return Path{ "/", "/", true };
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

        return Path{ path.substr(0, dirname_end), path.substr(basename_start), is_dir };
    }
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
