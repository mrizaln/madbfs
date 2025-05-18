#pragma once

#include "madbfs/common.hpp"

#include <cassert>
#include <print>

namespace madbfs::path
{
    class PathBuf;

    /**
     * @class Path
     * @brief Represent a file path in Linux system.
     *
     * This class is a simple wrapper over the underlying path string. It is like Str where it doesn't
     * allocate anything. Default constructed Path points to the root path.
     */
    class Path
    {
    public:
        friend class PathBuf;
        friend constexpr Opt<Path> create(Str path);

        constexpr Path() = default;

        constexpr bool is_root() const { return m_dirname == "/" and m_basename == "/"; }
        constexpr Str  filename() const { return m_basename; }
        constexpr Str  parent() const { return m_dirname; }
        constexpr Str  fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        /**
         * @brief Create a Path that points to parent as its basename.
         *
         * @return New Path.
         */
        constexpr Path parent_path() const
        {
            if (m_dirname == "/") {
                return Path{};
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
                return { { begin, begin + base_start + 1 }, { begin + base_start + 1, end } };
            }

            return { { begin, begin + dir_end + 1 }, { begin + base_start + 1, end } };
        }

        /**
         * @brief Create a new copy of the path and extend it with a name.
         *
         * @param name The name to extend with.
         *
         * @return A new PathBuf if extension is successful, std::nullopt otherwise.
         */
        Opt<PathBuf> extend_copy(Str name) const;

        /**
         * @brief Creates generator that iterates over the path.
         */
        Gen<Str> iter() const;

        /**
         * @brief Create a PathBuf from this Path.
         */
        PathBuf into_buf() const;

    private:
        constexpr Path(Str dirname, Str name)
            : m_dirname{ dirname }
            , m_basename{ name }
        {
        }

        Str m_dirname  = "/";
        Str m_basename = "/";
    };

    /**
     * @class PathBuf
     * @brief Represent a file path in Linux system that owns its path buffer.
     *
     * Default constructed PathBuf points to the root path.
     */
    class PathBuf
    {
    public:
        friend class Path;
        friend Opt<PathBuf> create_buf(String&& path);

        PathBuf() = default;

        static PathBuf root()
        {
            auto pathbuf  = PathBuf{};
            pathbuf.m_buf = String{ "/" };
            return pathbuf;
        }

        /**
         * @brief Extend the path with a name.
         *
         * @param name The name to extend with.
         *
         * @return True if extended, false if extension failed.
         *
         * Extension will fails if the name is empty, is '..' or is '.', or contains '/'.
         */
        bool extend(Str name);

        /**
         * @brief Create a new copy of the path and extend it with a name.
         *
         * @param name The name to extend with.
         *
         * @return A new PathBuf if extension is successful, std::nullopt otherwise.
         */
        Opt<PathBuf> extend_copy(Str name) const;

        Path as_path() const;

        operator Path() const { return as_path(); }

    private:
        String m_buf             = "/";
        usize  m_parent_size     = 1;
        usize  m_basename_size   = 1;
        usize  m_basename_offset = 0;
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

        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }
        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
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
            current = static_cast<usize>(it - path.begin());
            prev    = current;
        }

        const auto dirname_end = prev;

        // in case the basename contains repeated '/' like in the case '/home/user/documents/////note.md'
        auto basename_start = prev;
        while (path[basename_start] == '/') {
            ++basename_start;
        }

        return Path{ path.substr(0, dirname_end), path.substr(basename_start) };
    }

    /**
     * @brief Create a PathBuf from a string and owns the buffer.
     *
     * @param path_str The path to create from.
     */
    Opt<PathBuf> create_buf(String&& path_str);
}

namespace madbfs::path::inline literals
{
    namespace detail
    {
        template <usize N>
        struct FixedStr
        {
            char data[N]{};
            constexpr FixedStr() = default;
            constexpr FixedStr(const char (&str)[N]) { std::ranges::copy_n(str, N, data); }
            constexpr Str str() const { return { data, N - 1 }; }
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
