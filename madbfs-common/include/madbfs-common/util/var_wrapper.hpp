#pragma once

#include <variant>

namespace madbfs::util
{
    namespace detail
    {
        template <typename T, typename U>
        struct CopyCVRefTraits
        {
        private:
            using R  = std::remove_reference_t<T>;
            using U1 = std::conditional_t<std::is_const_v<R>, std::add_const_t<U>, U>;
            using U2 = std::conditional_t<std::is_volatile_v<R>, std::add_volatile_t<U1>, U1>;
            using U3 = std::conditional_t<std::is_lvalue_reference_v<T>, std::add_lvalue_reference_t<U2>, U2>;
            using U4 = std::conditional_t<std::is_rvalue_reference_v<T>, std::add_rvalue_reference_t<U3>, U3>;

        public:
            using Type = U4;
        };

        template <typename Src, typename Dest>
        using CopyCVRef = CopyCVRefTraits<Src, Dest>::Type;

        template <typename Visit, typename Var>
        static constexpr auto visitor_complete()
        {
            using BareVar        = std::remove_cvref_t<Var>;
            auto count_invocable = []<std::size_t... Is>(std::index_sequence<Is...>) {
                return (
                    std::invocable<Visit, detail::CopyCVRef<Var, std::variant_alternative_t<Is, BareVar>>>
                    + ... + 0
                );
            };

            static constexpr auto size = std::variant_size_v<BareVar>;

            if constexpr (size == count_invocable(std::make_index_sequence<size>{})) {
                return true;
            } else {
                static_assert(false, "not all variant cases are handled (visitor is not invocable)!");
                return false;
            }
        }

        template <typename Visit, typename Var>
        concept VisitorComplete = visitor_complete<Visit, Var>();
    }

    /**
     * @class VarWrapper
     * @brief CRTP struct for types that inherits `std::variant`
     *
     * This struct is useful for hiding template parameters of `std::variant` (make them opaque).
     */
    template <typename... Ts>
    struct VarWrapper : std::variant<Ts...>
    {
        using Var = std::variant<Ts...>;

        template <typename... Args>
            requires std::constructible_from<Var, Args...>
        VarWrapper(Args&&... args)
            : Var{ std::forward<Args>(args)... }
        {
        }

        template <typename T>
            requires (std::constructible_from<Ts, T> or ...)
        VarWrapper(T&& t)
            : Var{ std::forward<T>(t) }
        {
        }

        template <typename Self, detail::VisitorComplete<detail::CopyCVRef<Self, Var>> Visitor>
        decltype(auto) visit(this Self&& self, Visitor&& visitor)
        {
            return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).as_var());
        }

        template <typename Self>
        decltype(auto) as_var(this Self&& self)
        {
            using Var = detail::CopyCVRef<Self, Var>;
            return static_cast<Var>(self);
        }

        template <typename T>
        bool holds()
        {
            return std::holds_alternative<T>(as_var());
        }

        template <typename T>
        T* as()
        {
            auto& var = as_var();
            return std::get_if<T>(&var);
        }
    };
}
