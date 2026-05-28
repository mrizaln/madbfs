#pragma once

#include <type_traits>
namespace madbfs::util
{
    template <typename To, typename From>
    struct CopyConstTraits
    {
        using Type = std::conditional_t<std::is_const_v<std::remove_reference_t<From>>, const To, To>;
    };

    /**
     * @brief Helper traits for appplying constness of a type based on other type constness.
     *
     * @tparam To Type to be applied the const.
     * @tparam From Type for the source of the constness.
     *
     * This traits works on reference.
     */
    template <typename To, typename From>
    using CopyConst = CopyConstTraits<To, From>::Type;
}
