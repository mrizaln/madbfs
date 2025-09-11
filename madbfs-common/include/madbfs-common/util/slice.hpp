#pragma once

#include <madbfs-common/aliases.hpp>

#include <cassert>

namespace madbfs::util
{
    struct Slice
    {
        Str::size_type offset = 0;
        Str::size_type size   = 0;

        constexpr Str to_str(Str str) const { return str.substr(offset, size); }

        void keep_slice(String& str) const
        {
            assert(offset < str.size() and offset + size <= str.size());
            str.erase(offset + size);
            str.erase(0, offset);
        }
    };
}
