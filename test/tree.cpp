#include "adbfsm/path/path.hpp"
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

constexpr auto expected_rm = R"(
- /
    - hello/
        - foo.txt
        - movie.mp4
        - wife    ->    /bye/friends/work/loughshinny <3.txt
    - bye/
        - world.txt
        - movie.mp4
        - family/
            - dad.txt
            - mom.txt
        - friends/
            - school/
                - kal'tsit.txt
                - closure.txt
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

namespace mock
{
    using namespace adbfsm;
    using namespace adbfsm::data;

    class DummyConnection : public IConnection
    {
    public:
        Expect<std::generator<ParsedStat>> stat_dir(path::Path) override    // unused
        {
            return std::unexpected{ std::errc::inappropriate_io_control_operation };
        }

        Expect<ParsedStat> stat(path::Path path) override
        {
            return ParsedStat{ .stat = {}, .path = path.fullpath(), .link_to = {} };
        }

        Expect<void> touch(path::Path) override { return {}; }
        Expect<void> mkdir(path::Path) override { return {}; }
        Expect<void> rm(path::Path, bool) override { return {}; }
        Expect<void> rmdir(path::Path) override { return {}; }
        Expect<void> mv(path::Path, path::Path) override { return {}; }
        Expect<void> pull(path::Path, path::Path) override { return {}; }
        Expect<void> push(path::Path, path::Path) override { return {}; }
    };

    class DummyCache : public ICache
    {
    public:
        const Entry*         get(Id) const override { return nullptr; }
        Expect<const Entry*> add(IConnection&, path::Path) override { return nullptr; };
        bool                 remove(Id) override { return true; }
    };
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

        using adbfsm::path::operator""_path;

        auto connection = mock::DummyConnection{};
        auto cache      = mock::DummyCache{};
        auto context    = Node::Context{ connection, cache, "/dummy"_path };

        auto root = Node{ "/", nullptr, {}, Directory{} };

        auto* hello = root.mkdir("hello", context).unwrap();

        _ = hello->touch("world.txt", context).unwrap();
        _ = hello->touch("foo.txt", context).unwrap();
        _ = hello->touch("movie.mp4", context).unwrap();

        auto* bar = hello->mkdir("bar", context).unwrap();

        _ = bar->touch("baz.txt", context).unwrap();
        _ = bar->touch("qux.txt", context).unwrap();
        _ = bar->touch("quux.txt", context).unwrap();

        auto* bye = root.mkdir("bye", context).unwrap();

        _ = bye->touch("world.txt", context).unwrap();
        _ = bye->touch("movie.mp4", context).unwrap();
        _ = bye->touch("music.mp3", context).unwrap();

        auto* family = bye->mkdir("family", context).unwrap();

        _ = family->touch("dad.txt", context).unwrap();
        _ = family->touch("mom.txt", context).unwrap();

        auto* friends = bye->mkdir("friends", context).unwrap();

        _ = friends->touch("bob.txt", context).unwrap();

        auto* school = friends->mkdir("school", context).unwrap();

        _ = school->touch("kal'tsit.txt", context).unwrap();
        _ = school->touch("closure.txt", context).unwrap();

        auto* work = friends->mkdir("work", context).unwrap();

        auto* wife = work->touch("loughshinny <3.txt", context).unwrap();

        _ = work->touch("eblana?.mp4", context).unwrap();
        _ = school->link("hehe", work, context).unwrap();
        _ = hello->link("wife", wife, context).unwrap();
        _ = bye->touch("theresa.txt", context).unwrap();

        auto tree_str = fmt::format("\n{}", root);
        expect(expected == tree_str) << diff_str(expected, tree_str);

#undef unwrap
    };

    "constructed FileTree have same shape"_test = [&] {
        using namespace adbfsm::tree;

        auto connection = mock::DummyConnection{};
        auto cache      = mock::DummyCache{};

        auto tree = FileTree{ connection, cache };

#define unwrap() or_else([](auto e) { return raise_expect_error<void>(e); }).value()

        using adbfsm::path::operator""_path;

        tree.mkdir("/hello"_path).unwrap();
        tree.touch("/hello/world.txt"_path).unwrap();
        tree.touch("/hello/foo.txt"_path).unwrap();
        tree.touch("/hello/movie.mp4"_path).unwrap();
        tree.mkdir("/hello/bar"_path).unwrap();
        tree.touch("/hello/bar/baz.txt"_path).unwrap();
        tree.touch("/hello/bar/qux.txt"_path).unwrap();
        tree.touch("/hello/bar/quux.txt"_path).unwrap();
        tree.mkdir("/bye"_path).unwrap();
        tree.touch("/bye/world.txt"_path).unwrap();
        tree.touch("/bye/movie.mp4"_path).unwrap();
        tree.touch("/bye/music.mp3"_path).unwrap();
        tree.mkdir("/bye/family"_path).unwrap();
        tree.touch("/bye/family/dad.txt"_path).unwrap();
        tree.touch("/bye/family/mom.txt"_path).unwrap();
        tree.mkdir("/bye/friends"_path).unwrap();
        tree.touch("/bye/friends/bob.txt"_path).unwrap();
        tree.mkdir("/bye/friends/school"_path).unwrap();
        tree.touch("/bye/friends/school/kal'tsit.txt"_path).unwrap();
        tree.touch("/bye/friends/school/closure.txt"_path).unwrap();
        tree.mkdir("/bye/friends/work"_path).unwrap();
        tree.touch("/bye/friends/work/loughshinny <3.txt"_path).unwrap();
        tree.touch("/bye/friends/work/eblana?.mp4"_path).unwrap();

        tree.link("/bye/friends/school/hehe"_path, "/bye/friends/work"_path).unwrap();
        tree.link("/hello/wife"_path, "/bye/friends/work/loughshinny <3.txt"_path).unwrap();

        tree.touch("/bye/theresa.txt"_path).unwrap();

        auto tree_str = fmt::format("\n{}", tree.root());
        expect(expected == tree_str) << diff_str(expected, tree_str);

        tree.rm("/hello/world.txt"_path, false).unwrap();
        tree.rm("/hello/bar"_path, true).unwrap();
        tree.rm("/bye/music.mp3"_path, false).unwrap();
        tree.rm("/bye/friends/bob.txt"_path, false).unwrap();
        tree.rm("/bye/friends/school/hehe"_path, false).unwrap();

        tree_str = fmt::format("\n{}", tree.root());
        expect(expected_rm == tree_str) << diff_str(expected_rm, tree_str);

#undef unwrap
    };
}
