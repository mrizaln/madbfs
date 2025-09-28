#pragma once

#include <algorithm>
#include <cstddef>
#include <random>
#include <ranges>
#include <utility>
#include <variant>

namespace test::util
{
    template <typename Var, typename Fn>
    constexpr auto for_each_variant(Fn&& fn)
    {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (fn.template operator()<I, std::variant_alternative_t<I, Var>>(), ...);
        }(std::make_index_sequence<std::variant_size_v<std::decay_t<Var>>>());
    }

    template <std::ranges::contiguous_range... Rs>
    void shuffle(std::mt19937& rng, Rs&... rs)
    {
        using Dist  = std::uniform_int_distribution<std::size_t>;
        using Param = typename Dist::param_type;

        auto dist = Dist{};
        auto n    = std::min({ std::ranges::size(rs)... });

        for (auto i = std::size_t{ n - 1 }; i > 0; --i) {
            auto j = dist(rng, Param{ 0, i });
            (std::swap(rs[i], rs[j]), ...);
        }
    }
}
