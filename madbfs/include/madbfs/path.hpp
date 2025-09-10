#pragma once

#include <madbfs-common/aliases.hpp>

#include <fmt/format.h>

namespace madbfs::path
{
    class PathBuf;

    /**
     * @class Path
     * @brief Represent a file path in Linux system.
     *
     * This class is a simple wrapper over the underlying path string. It is like `Str` where it doesn't
     * allocate anything. Default constructed Path points to the root path.
     *
     * Default constructed `Path` points to the root path.
     *
     * This class only handles absolute paths, relative paths should be represented as plain string instead.
     */
    class Path
    {
    public:
        friend class PathBuf;
        friend constexpr Opt<Path> create(Str path);

        /**
         * @brief Default construct a path.
         *
         * The path will point to root.
         */
        constexpr Path() = default;

        /**
         * @brief Check whether the path points to root.
         */
        constexpr bool is_root() const { return m_dirname == "/" and m_basename == "/"; }

        /**
         * @brief Get the filename component of the path.
         */
        constexpr Str filename() const { return m_basename; }

        /**
         * @brief Get the the directory component of the path.
         *
         * This operation returns a directory path as if you do `dirname <path>` command. Use `parent_path()`
         * member function if you want the resulting path as `Path` instead of plain string.
         */
        constexpr Str parent() const { return m_dirname; }

        /**
         * @brief Get the full path as string.
         */
        constexpr Str str() const { return { m_dirname.begin(), m_basename.end() }; }

        /**
         * @brief Create a `Path` that points to parent as its basename.
         *
         * @return New Path.
         */
        constexpr Path parent_path() const;

        /**
         * @brief Create a new copy of the path and extend it with a name.
         *
         * @param name The name to extend with.
         *
         * @return A new `PathBuf` if extension is successful, `std::nullopt` otherwise.
         */
        Opt<PathBuf> extend_copy(Str name) const;

        /**
         * @brief Creates generator that iterates over the path components from root.
         */
        Gen<Str> iter() const;

        /**
         * @brief Create a `PathBuf` from this `Path`.
         */
        PathBuf owned() const;

        /**
         * @brief Convert `Path` into plain string.
         *
         * Same as calling `str()`.
         */
        constexpr operator Str() const { return str(); }

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
     * Default constructed `PathBuf` points to the root path.
     *
     * This class only handles absolute paths, relative paths should be represented as plain string instead.
     */
    class PathBuf
    {
    public:
        friend class Path;
        friend Opt<PathBuf> create_buf(String&& path);

        /**
         * @brief Default construct a path.
         *
         * The path will point to root.
         */
        PathBuf() = default;

        /**
         * @brief Check whether the path points to root.
         */
        bool is_root() const { return m_buf == "/"; }

        /**
         * @brief Get the filename component of the path.
         */
        Str filename() const { return { m_buf.data() + m_basename_offset, m_basename_size }; }

        /**
         * @brief Get the the directory component of the path.
         *
         * This operation returns a directory path as if you do `dirname <path>` command. Use `parent_path()`
         * member function if you want the resulting path as `Path` instead of plain string.
         */
        Str parent() const { return { m_buf.data(), m_parent_size }; }

        /**
         * @brief Get the full path as string.
         */
        Str str() const { return m_buf; }

        /**
         * @brief Create a `Path` that points to parent as its basename.
         *
         * @return New Path.
         */
        Path parent_path() const { return view().parent_path(); }

        /**
         * @brief Rename the filename.
         *
         * @param name The new name of the file.
         *
         * @return True if success.
         *
         * This function only rename the filename part of the file file unlike `rename` linux syscall that
         * basically replace the filepath altoghether. Renaming will fails if the name is empty, is '..' or is
         * '.', or contains '/'. If the path is root, rename will fail.
         */
        bool rename(Str name);

        /**
         * @brief Extend the path with a name.
         *
         * @param name The name to extend with.
         *
         * @return True if extended, false if extension failed.
         *
         * Extension will fail if the name is empty, is '..' or is '.', or contains '/'.
         */
        bool extend(Str name);

        /**
         * @brief Create a new copy of the path and extend it with a name.
         *
         * @param name The name to extend with.
         *
         * @return A new `PathBuf` if extension is successful, `std::nullopt` otherwise.
         */
        Opt<PathBuf> extend_copy(Str name) const;

        /**
         * @brief Creates generator that iterates over the path components from root.
         */
        Gen<Str> iter() const;

        /**
         * @brief Creates a `Path` from `PathBuf`.
         *
         * The backing `PathBuf` must outlive the constructed `Path`.
         */
        Path view() const;

        /**
         * @brief Convert `PathBuf` into `Path`.
         *
         * Same as calling `view()`, same rules apply as well.
         */
        operator Path() const& { return view(); }

        /**
         * @brief Convert `PathBuf` into plain string.
         *
         * Same as calling str, has same rule as `view()`.
         */
        operator Str() const& { return str(); }

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
    constexpr Opt<Path> create(Str path);

    /**
     * @brief Create a PathBuf from a string and owns the buffer.
     *
     * @param path_str The path to create from.
     */
    Opt<PathBuf> create_buf(String&& path_str);

    /**
     * @brief Resolve path relative to parent.
     *
     * @param parent Reference path.
     * @param path Path to be resolved.
     */
    PathBuf resolve(madbfs::path::Path parent, madbfs::Str path);
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
            constexpr FixedStr(const char (&str)[N]) { sr::copy_n(str, N, data); }
            constexpr Str str() const { return { data, N - 1 }; }
        };
    }

    /**
     * @brief Create a path at compile-time.
     */
    template <detail::FixedStr Str>
    consteval Path operator""_path()
    {
        if (auto path = create(Str.str()); path.has_value()) {
            return *path;
        }
        throw std::invalid_argument{ "Invalid path" };
    }
}

// impl
namespace madbfs::path
{
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

    constexpr Path Path::parent_path() const
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
}

template <>
struct fmt::formatter<madbfs::path::Path> : fmt::formatter<madbfs::Str>
{
    auto format(const madbfs::path::Path& path, auto& ctx) const
    {
        return fmt::formatter<madbfs::Str>::format(path.str(), ctx);
    }
};

template <>
struct fmt::formatter<madbfs::path::PathBuf> : fmt::formatter<madbfs::Str>
{
    auto format(const madbfs::path::PathBuf& path, auto& ctx) const
    {
        return fmt::formatter<madbfs::Str>::format(path.str(), ctx);
    }
};

static_assert(fmt::formattable<madbfs::path::Path>);
static_assert(fmt::formattable<madbfs::path::PathBuf>);
