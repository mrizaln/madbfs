#pragma once

#include "adbfsm/common.hpp"

#include <cassert>
#include <print>

namespace adbfsm::path
{
    /**
     * @class Path
     * @brief Represent a file path in Linux system.
     *
     * This class is just a simple wrapper like Str over the real underlying string type.
     */
    class Path
    {
    public:
        friend Opt<Path> create(Str path);

        Path() = default;

        Str filename() const { return m_basename; }
        Str parent() const { return m_dirname; }
        Str fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        bool is_root() const { return m_dirname == "/" and m_basename == ""; }

    private:
        Path(Str dirname, Str name)
            : m_dirname{ dirname }
            , m_basename{ name }
        {
        }

        Str m_dirname;    // can be empty string
        Str m_basename;
    };

    /**
     * @brief Create a Path from a string.
     *
     * If the path pased into this function is not absolute or empty, it will return `std::nullopt`.
     * Repeating '/' on the leading and trailing edge are ignored. If the repeating '/' is on the middle
     * however, it will be preserved.
     */
    inline Opt<Path> create(Str path)
    {
        if (path.size() == 0 or path.front() != '/') {
            return std::nullopt;
        }

        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
        }
        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }

        if (path == "/") {
            return Path{ "/", "" };
        }

        auto prev    = 1uz;
        auto current = 1uz;

        while (current < path.size()) {
            while (current < path.size() and path[current] == '/') {
                ++current;
            }
            current = path.find('/', current);
            if (current == Str::npos) {
                break;
            }
            prev = current;
        }

        const auto dirname_end = prev;

        // in case the basename contains repeated '//' like in the case '/home/user/documents/////note.md'
        auto basename_start = prev;
        while (path[basename_start] == '/') {
            ++basename_start;
        }

        return Path{ path.substr(0, dirname_end), path.substr(basename_start) };
    }
}
