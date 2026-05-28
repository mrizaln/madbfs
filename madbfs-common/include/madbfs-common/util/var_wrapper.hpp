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

        /**
         * @brief Helper traits for applying the same cvref of a type to another type.
         *
         * @tparam T Source type.
         * @tparam U Destination type.
         */
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

        /**
         * @brief Check whether a variant visitor is complete.
         *
         * @tparam Visit Visitor of the variant.
         * @tparam Var The variant type.
         */
        template <typename Visit, typename Var>
        concept VisitorComplete = visitor_complete<Visit, Var>();
    }

    template <typename>
    struct VarTraits
    {
    };

    /**
     * @brief Helper traits for variants.
     *
     * @tparam Ts Contained types of a variant.
     */
    template <template <typename...> typename VT, typename... Ts>
    struct VarTraits<VT<Ts...>>
    {
        /**
         * @brief Return the number of types contained inside the variant.
         */
        static consteval std::size_t size() { return sizeof...(Ts); }

        /**
         * @brief Check whether the variant contains a type.
         *
         * @tparam T The type to be checked.
         */
        template <typename T>
        static consteval bool has_type()
        {
            return (std::same_as<T, Ts> || ...);
        }

        /**
         * @brief Get the index of the type on the variant.
         *
         * @tparam T The type to be indexed.
         *
         * This function only works if the variant contains unique types only (no duplicates).
         */
        template <typename T>
            requires (has_type<T>())
        static consteval std::size_t type_index()
        {
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
                return ((std::same_as<T, Ts> ? Is : 0) + ...);
            }(std::make_index_sequence<size()>{});
        }

        /**
         * @brief Get type at index.
         */
        template <std::size_t I>
        using TypeAt = std::variant_alternative_t<I, std::variant<Ts...>>;

        /**
         * @brief Get the contained type of other variant at the same position as T of current variant.
         *
         * @tparam T Type to be swapped.
         * @tparam Var Other variant type.
         */
        template <typename T, typename Var>
        using Swap = VarTraits<Var>::template TypeAt<type_index<T>()>;
    };

    /**
     * @class VarWrapper
     *
     * @brief CRTP struct for types that inherits `std::variant`
     *
     * This struct is useful for hiding template parameters of `std::variant` (make them opaque).
     */
    template <typename... Ts>
    struct VarWrapper : std::variant<Ts...>
    {
        using Var    = std::variant<Ts...>;
        using Traits = VarTraits<Var>;

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

        /**
         * @brief Get index of type `T` in the variant at compile-time
         *
         * @tparam T The type in question.
         *
         * @return The index.
         */
        template <typename T>
        static consteval std::size_t index_of()
        {
            return Traits::template type_index<T>();
        }

        /**
         * @brief Visit the variant of this instance using provided visitor.
         *
         * @param visitor The visitor of the instance.
         */
        template <typename Self, detail::VisitorComplete<detail::CopyCVRef<Self, Var>> Visitor>
        decltype(auto) visit(this Self&& self, Visitor&& visitor)
        {
            return std::visit(std::forward<Visitor>(visitor), std::forward<Self>(self).as_var());
        }

        /**
         * @brief Return the underlying variant type.
         */
        template <typename Self>
        decltype(auto) as_var(this Self&& self)
        {
            using Var = detail::CopyCVRef<Self, Var>;
            return static_cast<Var>(self);
        }

        /**
         * @brief Check if variant contains a type.
         *
         * @tparam T The type to be checked.
         */
        template <typename T>
        bool holds() const
        {
            return std::holds_alternative<T>(as_var());
        }

        /**
         * @brief Get the contained variant of specified type.
         *
         * @tparam T Target type.
         *
         * @return The pointer to underlying type or `nullptr` if the variant is currently not containing the
         * specfieid type.
         */
        template <typename T>
        T* as()
        {
            auto& var = as_var();
            return std::get_if<T>(&var);
        }

        /**
         * @brief Get the contained variant of specified type.
         *
         * @tparam T Target type.
         *
         * @return The pointer to underlying type or `nullptr` if the variant is currently not containing the
         * specfieid type.
         */
        template <typename T>
        const T* as() const
        {
            auto& var = as_var();
            return std::get_if<T>(&var);
        }
    };
}
