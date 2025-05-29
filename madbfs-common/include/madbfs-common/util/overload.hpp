#pragma once

namespace madbfs::util
{
    template <typename... Fs>
    struct Overload : Fs...
    {
        using Fs::operator()...;
    };
}
