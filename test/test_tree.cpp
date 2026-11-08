#include <madbfs-common/util/split.hpp>
#include <madbfs/tree/tree.hpp>

#include <boost/ut.hpp>
#include <dtlx/dtlx.hpp>
#include <fmt/format.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <source_location>

namespace ut = boost::ext::ut;
using namespace madbfs::aliases;

// NOTE: alphabetical order per level
constexpr auto expected = R"(
- /
    - bye/
        - family/
            - dad.txt
            - mom.txt
        - friends/
            - bob.txt
            - school/
                - closure.txt
                - hehe  ->  /bye/friends/work
                - kal'tsit.txt
            - work/
                - eblana?.mp4
                - loughshinny <3.txt
        - movie.mp4
        - music.mp3
        - theresa.txt
        - world.txt
    - hello/
        - bar/
            - baz.txt
            - quux.txt
            - qux.txt
        - foo.txt
        - movie.mp4
        - wife  ->  /bye/friends/work/loughshinny <3.txt
        - world.txt
)";

// NOTE: alphabetical order per level
constexpr auto expected_rm = R"(
- /
    - bye/
        - family/
            - dad.txt
            - mom.txt
        - friends/
            - school/
                - closure.txt
                - kal'tsit.txt
            - work/
                - eblana?.mp4
                - loughshinny <3.txt
        - movie.mp4
        - theresa.txt
        - world.txt
    - hello/
        - foo.txt
        - movie.mp4
        - wife  ->  /bye/friends/work/loughshinny <3.txt
)";

// NOTE: since there is no ordering guarantee from the VFS, this formatter just order them alphabetically
template <>
struct fmt::formatter<madbfs::tree::Tree> : fmt::formatter<Str>
{
    auto format(const madbfs::tree::Tree& tree, auto&& ctx) const
    {
        using namespace madbfs;
        using namespace madbfs::tree;

        auto print_impl = [&](this auto&& self, Id id, usize depth) -> void {
            auto node = tree.get(id);
            if (not node) {
                return;
            }

            for (auto _ : sv::iota(0u, depth)) {
                fmt::format_to(ctx.out(), "    ");
            }

            auto extra = node->kind().visit(madbfs::Overload{
                [&](const node::Link& l) { return fmt::format("  ->  {}", l.target.value_or("[none]")); },
                [&](const node::Directory&) { return String{ "/" }; },
                [&](const auto&) { return String{ "" }; },
            });

            auto name = node->name() == "/" ? "" : node->name();
            fmt::format_to(ctx.out(), "- {}{}\n", name, extra);

            auto list = node->as_directory().transform([&](const auto& d) { return d.children(); });
            if (list) {
                auto nodes = *list | sr::to<Vec<Pair<String, Id>>>();
                sr::sort(nodes, std::less<>{}, [&](auto&& p) { return std::get<0>(p); });

                for (auto [_, id] : nodes) {
                    self(id, depth + 1);
                }
            };
        };

        print_impl(tree.root(), 0u);

        return ctx.out();
    }
};

class ExpectError : public std::runtime_error
{
public:
    ExpectError(Errc errc, std::source_location loc = std::source_location::current())
        : runtime_error{
            fmt::format("{}:{}:{} [{}]", loc.file_name(), loc.line(), loc.column(), madbfs::err_msg(errc))
        }
    {
    }
};

String diff_str(Str str1, Str str2)
{
    auto lines1 = madbfs::util::split(str1, '\n');
    auto lines2 = madbfs::util::split(str2, '\n');

    const auto red   = "\033[1;31m";
    const auto green = "\033[1;32m";
    const auto reset = "\033[0m";

    auto res = dtlx::diff(lines1, lines2);
    auto buf = String{};
    auto out = std::back_inserter(buf);

    for (auto [elem, info] : res.ses.get()) {
        switch (info.type) {
        case dtlx::SesEdit::Delete: fmt::format_to(out, "{}{}{}\n", red, elem, reset); break;
        case dtlx::SesEdit::Common: fmt::format_to(out, "{}\n", elem); break;
        case dtlx::SesEdit::Add: fmt::format_to(out, "{}{}{}\n", green, elem, reset); break;
        }
    }

    return buf;
}

int main()
{
    using namespace ut::literals;
    using ut::expect, ut::that;

    using namespace madbfs;
    using Reg = tree::node::Regular;
    using Dir = tree::node::Directory;
    using Lnk = tree::node::Link;

    spdlog::set_default_logger(spdlog::null_logger_mt("madbfs-test-tree"));
    // spdlog::set_level(spdlog::level::debug);

    "constructed Tree have same shape"_test = [&] {
        auto tree = tree::Tree{ "/", {} };
        auto root = tree.root();

        auto _ = std::ignore;

        auto hello = tree.create_node(root, "hello", Stat{}, Dir{}, false).value();
        auto world = tree.create_node(hello.id, "world.txt", Stat{}, Reg{}, false).value();
        _          = tree.create_node(hello.id, "foo.txt", Stat{}, Reg{}, false).value();
        _          = tree.create_node(hello.id, "movie.mp4", Stat{}, Reg{}, false).value();

        auto bar = tree.create_node(hello.id, "bar", Stat{}, Dir{}, false).value();
        _        = tree.create_node(bar.id, "baz.txt", Stat{}, Reg{}, false).value();
        _        = tree.create_node(bar.id, "qux.txt", Stat{}, Reg{}, false).value();
        _        = tree.create_node(bar.id, "quux.txt", Stat{}, Reg{}, false).value();

        auto bye   = tree.create_node(root, "bye", Stat{}, Dir{}, false).value();
        _          = tree.create_node(bye.id, "world.txt", Stat{}, Reg{}, false).value();
        _          = tree.create_node(bye.id, "movie.mp4", Stat{}, Reg{}, false).value();
        auto music = tree.create_node(bye.id, "music.mp3", Stat{}, Reg{}, false).value();

        auto family = tree.create_node(bye.id, "family", Stat{}, Dir{}, false).value();
        _           = tree.create_node(family.id, "dad.txt", Stat{}, Reg{}, false).value();
        _           = tree.create_node(family.id, "mom.txt", Stat{}, Reg{}, false).value();

        auto friends = tree.create_node(bye.id, "friends", Stat{}, Dir{}, false).value();
        auto bob     = tree.create_node(friends.id, "bob.txt", Stat{}, Reg{}, false).value();

        auto school = tree.create_node(friends.id, "school", Stat{}, Dir{}, false).value();
        _           = tree.create_node(school.id, "kal'tsit.txt", Stat{}, Reg{}, false).value();
        _           = tree.create_node(school.id, "closure.txt", Stat{}, Reg{}, false).value();

        auto work = tree.create_node(friends.id, "work", Stat{}, Dir{}, false).value();
        _         = tree.create_node(work.id, "loughshinny <3.txt", Stat{}, Reg{}, false).value();
        _         = tree.create_node(work.id, "eblana?.mp4", Stat{}, Reg{}, false).value();

        auto link_1 = Lnk{ .target = "/bye/friends/work" };
        auto hehe   = tree.create_node(school.id, "hehe", Stat{}, link_1).value();

        auto link_2 = Lnk{ .target = "/bye/friends/work/loughshinny <3.txt" };
        _           = tree.create_node(hello.id, "wife", Stat{}, link_2).value();

        _ = tree.create_node(bye.id, "theresa.txt", Stat{}, Reg{}, false).value();

        auto tree_str = fmt::format("\n{}", tree);
        expect(expected == tree_str) << diff_str(expected, tree_str);

        tree.remove(world.id).value();
        tree.remove(music.id).value();
        tree.remove(bob.id).value();
        tree.remove(hehe.id).value();

        // bar reference may have been invalidated, thus refetching using the id
        auto& bar_node = tree.get(bar.id).value();
        auto& bar_dir  = bar_node.as_directory().value();

        for (auto&& [name, id] : std::as_const(bar_dir).children()) {
            tree.remove(id).value();
        }
        tree.remove(bar.id).value();

        tree_str = fmt::format("\n{}", tree);
        expect(expected_rm == tree_str) << diff_str(expected_rm, tree_str);
    };
}
