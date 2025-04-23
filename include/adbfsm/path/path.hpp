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
     * This class is just a simple wrapper like Str over the real underlying string type.
     */
    class Path
    {
    public:
        friend Opt<Path> create(Str path);

        Path() = default;

        bool is_root() const { return m_dirname == "/" and m_basename == "/"; }

        Str filename() const { return m_basename; }
        Str parent() const { return m_dirname; }
        Str fullpath() const { return { m_dirname.begin(), m_basename.end() }; }

        /**
         * @brief Creates generator that iterates over the path.
         */
        std::generator<Str> iter() const;

        /**
         * @brief Creates generator that iterates over the path up till the parent directory.
         */
        std::generator<Str> iter_parent() const;

    private:
        Path(Str dirname, Str name)
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
    Opt<Path> create(Str path);

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
