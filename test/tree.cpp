#include "adbfsm/tree/node.hpp"
#include "adbfsm/tree/util.hpp"
#include <adbfsm/tree/file_tree.hpp>

#include <boost/ut.hpp>
#include <dtl_modern/dtl_modern.hpp>
#include <dtl_modern/extra/ses_display_pretty.hpp>
#include <fmt/base.h>
#include <fmt/format.h>

#include <source_location>

namespace ut = boost::ext::ut;
using namespace adbfsm::aliases;

// NOTE: extra newline at the start of this string is intentional (make sure to take it into account)
constexpr auto expected = R"(
- /
    - hello/
        - world.txt
        - foo.txt
        - movie.mp4
        - bar/
            - baz.txt
            - qux.txt
            - quux.txt
        - wife    ->    /bye/friends/work/loughshinny <3.txt
    - bye/
        - world.txt
        - movie.mp4
        - music.mp3
        - family/
            - dad.txt
            - mom.txt
        - friends/
            - bob.txt
            - school/
                - kal'tsit.txt
                - closure.txt
                - hehe    ->    /bye/friends/work
            - work/
                - loughshinny <3.txt
                - eblana?.mp4
        - theresa.txt
)";

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
            auto name = node->name() == "/" ? "" : node->name();
            fmt::format_to(ctx.out(), "- {}{}\n", name, additional);

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

class ExpectError : public std::runtime_error
{
public:
    ExpectError(std::errc errc, std::source_location loc = std::source_location::current())
        : runtime_error{ fmt::format(
              "{}:{}:{} [{}]",
              loc.file_name(),
              loc.line(),
              loc.column(),
              std::make_error_code(errc).message()
          ) }
    {
    }
};

template <typename T>
Expect<T> raise_expect_error(std::errc errc, std::source_location loc = std::source_location::current())
{
    throw ExpectError{ errc, loc };
}

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

    "constructed tree from raw node have same shape"_test = [&] {
        using namespace adbfsm::tree;

#define unwrap() or_else([](auto e) { return raise_expect_error<Node*>(e); }).value()

        auto root = Node{ "/", nullptr, Directory{} };

        auto* hello = root.mkdir("hello").unwrap();

        _ = hello->touch("world.txt").unwrap();
        _ = hello->touch("foo.txt").unwrap();
        _ = hello->touch("movie.mp4").unwrap();

        auto* bar = hello->mkdir("bar").unwrap();

        _ = bar->touch("baz.txt").unwrap();
        _ = bar->touch("qux.txt").unwrap();
        _ = bar->touch("quux.txt").unwrap();

        auto* bye = root.mkdir("bye").unwrap();

        _ = bye->touch("world.txt").unwrap();
        _ = bye->touch("movie.mp4").unwrap();
        _ = bye->touch("music.mp3").unwrap();

        auto* family = bye->mkdir("family").unwrap();

        _ = family->touch("dad.txt").unwrap();
        _ = family->touch("mom.txt").unwrap();

        auto* friends = bye->mkdir("friends").unwrap();

        _ = friends->touch("bob.txt").unwrap();

        auto* school = friends->mkdir("school").unwrap();

        _ = school->touch("kal'tsit.txt").unwrap();
        _ = school->touch("closure.txt").unwrap();

        auto* work = friends->mkdir("work").unwrap();

        auto* wife = work->touch("loughshinny <3.txt").unwrap();

        _ = work->touch("eblana?.mp4").unwrap();
        _ = school->link("hehe", work).unwrap();
        _ = hello->link("wife", wife).unwrap();
        _ = bye->touch("theresa.txt").unwrap();

        // NOTE: taking into account the extra newline at the start
        auto tree_str = fmt::format("\n{}", root);

        expect(expected == tree_str) << diff_str(expected, tree_str);

#undef unwrap
    };

    "constructed FileTree have same shape"_test = [&] {
        using namespace adbfsm::tree;

        auto tree = FileTree{};

#define unwrap() or_else([](auto e) { return raise_expect_error<void>(e); }).value()

        tree.mkdir("/hello").unwrap();
        tree.touch("/hello/world.txt").unwrap();
        tree.touch("/hello/foo.txt").unwrap();
        tree.touch("/hello/movie.mp4").unwrap();
        tree.mkdir("/hello/bar").unwrap();
        tree.touch("/hello/bar/baz.txt").unwrap();
        tree.touch("/hello/bar/qux.txt").unwrap();
        tree.touch("/hello/bar/quux.txt").unwrap();
        tree.mkdir("/bye").unwrap();
        tree.touch("/bye/world.txt").unwrap();
        tree.touch("/bye/movie.mp4").unwrap();
        tree.touch("/bye/music.mp3").unwrap();
        tree.mkdir("/bye/family").unwrap();
        tree.touch("/bye/family/dad.txt").unwrap();
        tree.touch("/bye/family/mom.txt").unwrap();
        tree.mkdir("/bye/friends").unwrap();
        tree.touch("/bye/friends/bob.txt").unwrap();
        tree.mkdir("/bye/friends/school").unwrap();
        tree.touch("/bye/friends/school/kal'tsit.txt").unwrap();
        tree.touch("/bye/friends/school/closure.txt").unwrap();
        tree.mkdir("/bye/friends/work").unwrap();
        tree.touch("/bye/friends/work/loughshinny <3.txt").unwrap();
        tree.touch("/bye/friends/work/eblana?.mp4").unwrap();

        tree.link("/bye/friends/school/hehe", "/bye/friends/work").unwrap();
        tree.link("/hello/wife", "/bye/friends/work/loughshinny <3.txt").unwrap();

        tree.touch("/bye/theresa.txt").unwrap();

        // NOTE: taking into account the extra newline at the start
        auto tree_str = fmt::format("\n{}", tree.root());

        expect(expected == tree_str) << diff_str(expected, tree_str);

#undef unwrap
    };
}
