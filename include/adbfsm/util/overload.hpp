#pragma once

namespace adbfsm::util
{
    template <typename... Fs>
    struct Overload : Fs...
    {
        using Fs::operator()...;
    };
}
