#pragma once

#include "adbfsm/common.hpp"

#include <cassert>
#include <print>

namespace adbfsm::path
{
    class PathBuf;

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
        friend PathBuf             combine(Path path1, Path path2);
        friend Opt<PathBuf>        combine(Path path, Str name);

        constexpr Path() = default;

        static constexpr Path root() { return { "/", "/" }; }

        constexpr bool is_root() const { return m_dirname == "/" and m_basename == "/"; }

        constexpr Str filename() const { return m_basename; }
        constexpr Str parent() const { return m_dirname; }
        constexpr Str fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        constexpr Path parent_path() const
        {
            if (m_dirname == "/") {
                return root();
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
         * @brief Creates generator that iterates over the path.
         */
        Gen<Str> iter() const;

        PathBuf into_buf() const;

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
     * @class PathBuf
     * @brief Represent a file path in Linux system that owns its path buffer.
     */
    class PathBuf
    {
    public:
        friend class Path;
        friend PathBuf      combine(Path path1, Path path2);
        friend Opt<PathBuf> combine(Path path, Str name);
        friend Opt<PathBuf> create_buf(String&& path);

        PathBuf() = default;

        static PathBuf root()
        {
            auto pathbuf  = PathBuf{};
            pathbuf.m_buf = String{ "/" };
            return pathbuf;
        }

        Path as_path() const;

    private:
        String m_buf;
        usize  m_parent_size     = 0;
        usize  m_basename_size   = 0;
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

        // in case the basename contains repeated '//' like in the case '/home/user/documents/////note.md'
        auto basename_start = prev;
        while (path[basename_start] == '/') {
            ++basename_start;
        }

        return Path{ path.substr(0, dirname_end), path.substr(basename_start) };
    }

    Opt<PathBuf> create_buf(String&& path_str);

    /**
     * @brief Combine two paths into one.
     *
     * @param path1 First path.
     * @param path2 Second path.
     * @return Combined path as PathBuf.
     */
    PathBuf combine(Path path1, Path path2);

    /*
     * @brief Combine a path with a name.
     *
     * @param path Path to combine with.
     * @param name Name to combine with.
     *
     * @return Combined path as PathBuf.
     *
     * The name must not contain '/', if it does, it will return `std::nullopt`.
     */
    Opt<PathBuf> combine(Path path, Str name);
}

namespace adbfsm::path::inline literals
{
    namespace detail
    {
        template <usize N>
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
