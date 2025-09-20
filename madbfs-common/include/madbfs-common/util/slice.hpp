#pragma once

#include <madbfs-common/aliases.hpp>

#include <cassert>

namespace madbfs::util
{
    /**
     * @class Slice
     *
     * @brief Slice of "something" only pointed by offset and size.
     *
     * Mainly used for `std::string_view` but can be used for anything really.
     */
    struct Slice
    {
        Str::size_type offset = 0;
        Str::size_type size   = 0;

        /**
         * @brief Slice string view.
         *
         * @param str Input string.
         *
         * @return The slice.
         */
        constexpr Str to_str(Str str) const { return str.substr(offset, size); }

        /**
         * @brief Modify string and only keep the part of the string pointed by slice.
         *
         * @param str Input string.
         */
        void keep_slice(String& str) const
        {
            assert(offset < str.size() and offset + size <= str.size());
            str.erase(offset + size);
            str.erase(0, offset);
        }
    };
}
