#pragma once

#include <type_traits>
namespace madbfs::util
{
    template <typename To, typename From>
    struct CopyConstTraits
    {
        using Type = std::conditional_t<std::is_const_v<std::remove_reference_t<From>>, const To, To>;
    };

    template <typename To, typename From>
    using CopyConst = CopyConstTraits<To, From>::Type;
}
