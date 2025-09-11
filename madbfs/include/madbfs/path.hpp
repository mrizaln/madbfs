#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/util/slice.hpp>

#include <fmt/format.h>

// NOTE: I ended up reimplementing `std::filesystem::path` lol, but since std is missing the `path_view` I
// think it's worth it. Also I want to avoid mistaking the path used by the filesystem as path in the host.
// Creating a separate encapsulation helps avoid that.

namespace madbfs::path
{
    using util::Slice;

    class Path;
    class PathBuf;
    struct SemiPath;

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
        friend Opt<SemiPath> create(Str path);

        /**
         * @brief Default construct a path.
         *
         * The path will point to root.
         */
        Path() = default;

        /**
         * @brief Check whether the path points to root.
         */
        bool is_root() const { return m_components.empty(); }

        /**
         * @brief Get the filename component of the path.
         */
        Str filename() const { return is_root() ? "/" : m_components.back().to_str(m_path); }

        /**
         * @brief Get the the directory component of the path.
         *
         * This operation returns a directory path as if you do `dirname <path>` command. Use `parent_path()`
         * member function if you want the resulting path as `Path` instead of plain string.
         */
        Str parent() const
        {
            if (is_root() or m_components.size() == 1) {
                return "/";
            }

            const auto parent = m_components[m_components.size() - 2];
            const auto size   = parent.offset + parent.size;

            return { m_path.data(), size };
        }

        /**
         * @brief Create a `Path` that points to parent as its basename.
         *
         * @return New Path.
         */
        Path parent_path() const
        {
            return is_root() ? *this : Path{ parent(), { m_components.begin(), m_components.size() - 1 } };
        }

        /**
         * @brief Get the full path as string.
         */
        Str str() const { return m_path; }

        /**
         * @brief Create a new copy of the path and extend it with a name.
         *
         * @param name The name to extend with.
         *
         * @return A new `PathBuf` if extension is successful, `std::nullopt` otherwise.
         */
        Opt<PathBuf> extend_copy(Str name) const;

        /**
         * @brief Create a `PathBuf` from this `Path`.
         */
        PathBuf owned() const;

        /**
         * @brief Iterate the path components.
         *
         * The root path '/' is not included in the components. A path that points to root will have zero
         * components.
         */
        FRange auto iter() const { return m_components | sv::transform(proj(&Slice::to_str, m_path)); }

        /**
         * @brief Convert `Path` into plain string.
         *
         * Same as calling `str()`.
         */
        operator Str() const { return str(); }

    private:
        Path(Str path, Span<const Slice> components)
            : m_path{ path }
            , m_components{ components }
        {
        }

        Str               m_path       = "/";
        Span<const Slice> m_components = {};    // zero if points to root
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
        bool is_root() const { return m_components.empty(); }

        /**
         * @brief Get the filename component of the path.
         */
        Str filename() const { return is_root() ? "/" : m_components.back().to_str(m_path); }

        /**
         * @brief Get the the directory component of the path.
         *
         * This operation returns a directory path as if you do `dirname <path>` command. Use `parent_path()`
         * member function if you want the resulting path as `Path` instead of plain string.
         */
        Str parent() const
        {
            if (is_root() or m_components.size() == 1) {
                return "/";
            }

            const auto parent = m_components[m_components.size() - 2];
            const auto size   = parent.offset + parent.size;

            return { m_path.data(), size };
        }

        /**
         * @brief Create a `Path` that points to parent as its basename.
         *
         * @return New Path.
         */
        Path parent_path() const
        {
            return is_root() ? *this : Path{ parent(), { m_components.begin(), m_components.size() - 1 } };
        }

        /**
         * @brief Get the full path as string.
         */
        Str str() const { return m_path; }

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
         * @brief Creates a `Path` from `PathBuf`.
         *
         * The backing `PathBuf` must outlive the constructed `Path`.
         */
        Path view() const;

        /**
         * @brief Iterate the path components.
         *
         * The root path '/' is not included in the components. A path that points to root will have zero
         * components.
         */
        FRange auto iter() const { return m_components | sv::transform(proj(&Slice::to_str, m_path)); }

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
        PathBuf(String&& path, Vec<Slice>&& components)
            : m_path{ std::move(path) }
            , m_components{ std::move(components) }
        {
        }

        String     m_path       = "/";
        Vec<Slice> m_components = {};
    };

    /**
     * @class SemiPath
     * @brief Like `PathBuf` but only own the components vector, not the path string.
     *
     * This struct is convertible to `Path` to make it easier to use in place of `Path` as arguments.
     */
    struct SemiPath
    {
        Vec<Slice> components = {};
        Path       path       = {};

        operator Path() const& { return path; }
    };

    /**
     * @brief Create a Path from a string.
     *
     * @param path Path string.
     *
     * If the path pased into this function is not absolute or empty, it will return `std::nullopt`.
     * Repeating '/' on the leading and trailing edge are ignored. If the repeating '/' is on the middle
     * however, it will be preserved.
     */
    Opt<SemiPath> create(Str path);

    /**
     * @brief Create a PathBuf from a string and owns the buffer.
     *
     * @param path Path string.
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
    template <detail::FixedStr StrValue>
    PathBuf operator""_path()
    {
        if (auto path = create_buf(String{ StrValue.str() }); path.has_value()) {
            return std::move(path).value();
        }
        throw std::invalid_argument{ "Invalid path" };
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
