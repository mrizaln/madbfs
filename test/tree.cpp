#include "adbfsm/tree/node.hpp"
#include "adbfsm/tree/util.hpp"
#include <adbfsm/tree/file_tree.hpp>

#include <boost/ut.hpp>
#include <dtl_modern/dtl_modern.hpp>
#include <dtl_modern/extra/ses_display_pretty.hpp>
#include <fmt/base.h>
#include <fmt/format.h>

namespace ut = boost::ext::ut;

using namespace adbfsm::aliases;

template <>
struct fmt::formatter<adbfsm::tree::Node> : fmt::formatter<std::string_view>
{
    auto format(const adbfsm::tree::Node& node, auto&& ctx) const
    {
        using namespace adbfsm::tree;

        auto print_impl = [&](this auto&& self, const Node* node, usize depth) -> void {
            if (node == nullptr) {
                return;
            }

            for (auto _ : sv::iota(0u, depth)) {
                fmt::format_to(ctx.out(), "    ");
            }

            auto additional = node->visit(util::Overload{
                [&](const Link& link) { return fmt::format("    ->    {}", link.target()->build_path()); },
                [&](const Directory&) { return String{ "/" }; },
                [&](const auto&) { return String{ "" }; },
            });

            // if root, don't print the name since additional will print dir mark (/)
            auto name = node->name == "/" ? "" : node->name;
            fmt::format_to(ctx.out(), "- [{}] {}{}\n", node->id, name, additional);

            node->visit(util::Overload{
                [&](const Directory& dir) {
                    for (const auto& child : dir.children()) {
                        self(child.get(), depth + 1);
                    }
                },
                [&](const auto&) { /* do nothing */ },
            });
        };

        print_impl(&node, 0u);

        return ctx.out();
    }
};

String diff_str(Str str1, Str str2)
{
    auto res   = dtl_modern::diff(str1, str2);
    auto left  = fmt::format("{:l}", dtl_modern::extra::display_pretty(res.m_ses));
    auto right = fmt::format("{:r}", dtl_modern::extra::display_pretty(res.m_ses));

    auto sep = [](Str name) { return fmt::format("{:-^80}\n", name); };

    return '\n' + sep("[ expect ]") + left + sep("[ actual ]") + right;
}

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::that;

    auto _ = std::ignore;

    "constructed tree have same shape"_test = [&] {
        using namespace adbfsm::tree;

        // NOTE: extra newline at the start of this string is intentional (make sure to take it into account)
        constexpr auto expected = R"(
- [1] /
    - [2] hello/
        - [3] world.txt
        - [4] foo.txt
        - [5] movie.mp4
        - [6] bar/
            - [7] baz.txt
            - [8] qux.txt
            - [9] quux.txt
        - [26] wife    ->    /bye/friends/work/loughshinny <3.txt
    - [10] bye/
        - [11] world.txt
        - [12] movie.mp4
        - [13] music.mp3
        - [14] family/
            - [15] dad.txt
            - [16] mom.txt
        - [17] friends/
            - [18] bob.txt
            - [19] school/
                - [20] kal'tsit.txt
                - [21] closure.txt
                - [25] hehe    ->    /bye/friends/work
            - [22] work/
                - [23] loughshinny <3.txt
                - [24] eblana?.mp4
        - [27] theresa.txt
)";

        auto root = Node{ "/", nullptr, Directory{} };

        auto* hello = root.mkdir("hello").value();

        _ = hello->touch("world.txt");
        _ = hello->touch("foo.txt");
        _ = hello->touch("movie.mp4");

        auto* bar = hello->mkdir("bar").value();

        _ = bar->touch("baz.txt");
        _ = bar->touch("qux.txt");
        _ = bar->touch("quux.txt");

        auto* bye = root.mkdir("bye").value();

        _ = bye->touch("world.txt");
        _ = bye->touch("movie.mp4");
        _ = bye->touch("music.mp3");

        auto* family = bye->mkdir("family").value();

        _ = family->touch("dad.txt");
        _ = family->touch("mom.txt");

        auto* friends = bye->mkdir("friends").value();

        _ = friends->touch("bob.txt");

        auto* school = friends->mkdir("school").value();

        _ = school->touch("kal'tsit.txt");
        _ = school->touch("closure.txt");

        auto* work = friends->mkdir("work").value();

        auto* wife = work->touch("loughshinny <3.txt").value();

        _ = work->touch("eblana?.mp4");

        _ = school->link("hehe", work);

        _ = hello->link("wife", wife);

        _ = bye->touch("theresa.txt");

        // NOTE: taking into account the extra newline at the start
        auto tree_str = fmt::format("\n{}", root);

        expect(expected == tree_str) << diff_str(expected, tree_str);
    };
}
