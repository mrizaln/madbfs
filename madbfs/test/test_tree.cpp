#include "madbfs/path.hpp"
#include "madbfs/tree/file_tree.hpp"
#include "madbfs/tree/node.hpp"

#include <madbfs-common/util/overload.hpp>
#include <madbfs-common/util/split.hpp>

#include <boost/ut.hpp>
#include <dtl_modern/dtl_modern.hpp>
#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/format.h>

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
                - hehe    ->    /bye/friends/work
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
        - wife    ->    /bye/friends/work/loughshinny <3.txt
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
        - wife    ->    /bye/friends/work/loughshinny <3.txt
)";

// NOTE: since there is no ordering guarantee from the VFS, this formatter just order them alphabetically
template <>
struct fmt::formatter<madbfs::tree::Node> : fmt::formatter<Str>
{
    auto format(const madbfs::tree::Node& node, auto&& ctx) const
    {
        using namespace madbfs::tree;

        auto print_impl = [&](this auto&& self, const Node* node, usize depth) -> void {
            if (node == nullptr) {
                return;
            }

            for (auto _ : sv::iota(0u, depth)) {
                fmt::format_to(ctx.out(), "    ");
            }

            auto visitor = madbfs::util::Overload{
                [&](const node::Link& link) { return fmt::format("    ->    {}", link.target); },
                [&](const node::Directory&) { return String{ "/" }; },
                [&](const auto&) { return String{ "" }; },
            };
            auto additional = std::visit(visitor, node->value());

            // if root, don't print the name since additional will print dir mark (/)
            auto name = node->name() == "/" ? "" : node->name();
            fmt::format_to(ctx.out(), "- {}{}\n", name, additional);

            if (auto* dir = std::get_if<node::Directory>(&node->value())) {
                auto to_ref = [](const Uniq<Node>& f) { return std::ref(*f); };
                auto ptrs   = dir->children() | sv::transform(to_ref) | sr::to<std::vector>();
                sr::sort(ptrs, std::less<>{}, &Node::name);
                for (const Node& child : ptrs) {
                    self(&child, depth + 1);
                }
            };
        };

        print_impl(&node, 0u);

        return ctx.out();
    }
};

class ExpectError : public std::runtime_error
{
public:
    ExpectError(Errc errc, std::source_location loc = std::source_location::current())
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
[[noreturn]] madbfs::Unit raise_expect_error(
    Errc                 errc,
    std::source_location loc = std::source_location::current()
)
{
    throw ExpectError{ errc, loc };
}

String diff_str(Str str1, Str str2)
{
    auto lines1 = madbfs::util::split(str1, '\n');
    auto lines2 = madbfs::util::split(str2, '\n');

    const auto red   = fmt::fg(fmt::color::red);
    const auto green = fmt::fg(fmt::color::green);

    auto res = dtl_modern::diff(lines1, lines2);
    auto buf = String{};
    auto out = std::back_inserter(buf);

    for (auto [elem, info] : res.m_ses.get()) {
        switch (info.m_type) {
        case dtl_modern::SesEdit::Delete: fmt::format_to(out, red, "{}\n", elem); break;
        case dtl_modern::SesEdit::Common: fmt::format_to(out, "{}\n", elem); break;
        case dtl_modern::SesEdit::Add: fmt::format_to(out, green, "{}\n", elem); break;
        }
    }

    return buf;
}

namespace mock
{
    using namespace madbfs;
    using namespace madbfs::connection;
    using data::Stat;
    using path::Path;
    using path::PathBuf;

    class DummyConnection final : public Connection
    {
    public:
        using Stats = Gen<ParsedStat>;

        Str name() const override { return "dummy"; }

        AExpect<Stats>  statdir(Path) override { co_return Unexpect{ {} }; }
        AExpect<Stat>   stat(Path) override { co_return Stat{}; }
        AExpect<String> readlink(Path path) override { co_return path.fullpath(); };
        AExpect<void>   mknod(Path, mode_t, dev_t) override { co_return Expect<void>{}; }
        AExpect<void>   mkdir(Path, mode_t) override { co_return Expect<void>{}; }
        AExpect<void>   unlink(Path) override { co_return Expect<void>{}; }
        AExpect<void>   rmdir(Path) override { co_return Expect<void>{}; }
        AExpect<void>   rename(Path, path::Path, u32) override { co_return Expect<void>{}; }
        AExpect<void>   truncate(Path, off_t) override { co_return Expect<void>{}; }
        AExpect<usize>  read(Path, Span<char>, off_t) override { co_return Expect<usize>{}; }
        AExpect<usize>  write(Path, Span<const char>, off_t) override { co_return Expect<usize>{}; }
        AExpect<void>   utimens(Path, timespec, timespec) override { co_return Expect<void>{}; }
        AExpect<usize>  copy_file_range(Path, off_t, Path, off_t, usize size) override { co_return size; }
    };
}

int main()
{
    using namespace ut::literals;
    using ut::expect, ut::that;

    auto _ = std::ignore;

    "constructed tree from raw node have same shape"_test = [&] {
        using namespace madbfs::tree;

#define unwrap() transform_error([](auto e) { return raise_expect_error<Node*>(e); }).value()

        using madbfs::path::operator""_path;

        auto connection = mock::DummyConnection{};
        auto cache      = madbfs::data::Cache{ connection, 64 * 1024, 1024 };
        auto counter    = std::atomic<u64>{};

        // NOTE: operations like mknod and mkdir only considers filename
        // NOTE: full path only matter for connection and caching
        auto dummy = madbfs::path::PathBuf::root();
        auto path  = madbfs::path::Path::root();

        auto make_context = [&](Str name) {
            dummy.extend(name);
            path = dummy.as_path();
            return Node::Context{ connection, cache, counter, path };
        };

        auto io_context = madbfs::async::Context{};

        auto coro = [&] -> madbfs::Await<void> {
            auto root = Node{ "/", nullptr, {}, node::Directory{} };

            Node& hello = (co_await root.mkdir(make_context("hello"), 0)).unwrap();

            _ = (co_await hello.mknod(make_context("world.txt"), 0, 0)).unwrap();
            _ = (co_await hello.mknod(make_context("foo.txt"), 0, 0)).unwrap();
            _ = (co_await hello.mknod(make_context("movie.mp4"), 0, 0)).unwrap();

            Node& bar = (co_await hello.mkdir(make_context("bar"), 0)).unwrap();

            _ = (co_await bar.mknod(make_context("baz.txt"), 0, 0)).unwrap();
            _ = (co_await bar.mknod(make_context("qux.txt"), 0, 0)).unwrap();
            _ = (co_await bar.mknod(make_context("quux.txt"), 0, 0)).unwrap();

            Node& bye = (co_await root.mkdir(make_context("bye"), 0)).unwrap();

            _ = (co_await bye.mknod(make_context("world.txt"), 0, 0)).unwrap();
            _ = (co_await bye.mknod(make_context("movie.mp4"), 0, 0)).unwrap();
            _ = (co_await bye.mknod(make_context("music.mp3"), 0, 0)).unwrap();

            Node& family = (co_await bye.mkdir(make_context("family"), 0)).unwrap();

            _ = (co_await family.mknod(make_context("dad.txt"), 0, 0)).unwrap();
            _ = (co_await family.mknod(make_context("mom.txt"), 0, 0)).unwrap();

            Node& friends = (co_await bye.mkdir(make_context("friends"), 0)).unwrap();

            _ = (co_await friends.mknod(make_context("bob.txt"), 0, 0)).unwrap();

            Node& school = (co_await friends.mkdir(make_context("school"), 0)).unwrap();

            _ = (co_await school.mknod(make_context("kal'tsit.txt"), 0, 0)).unwrap();
            _ = (co_await school.mknod(make_context("closure.txt"), 0, 0)).unwrap();

            Node& work = (co_await friends.mkdir(make_context("work"), 0)).unwrap();

            Node& wife = (co_await work.mknod(make_context("loughshinny <3.txt"), 0, 0)).unwrap();

            _ = (co_await work.mknod(make_context("eblana?.mp4"), 0, 0)).unwrap();
            _ = school.symlink("hehe", work.build_path().as_path().fullpath()).unwrap();
            _ = hello.symlink("wife", wife.build_path().as_path().fullpath()).unwrap();
            _ = (co_await bye.mknod(make_context("theresa.txt"), 0, 0)).unwrap();

            auto tree_str = fmt::format("\n{}", root);
            expect(expected == tree_str) << diff_str(expected, tree_str);
        };

        madbfs::async::spawn(io_context, coro(), madbfs::async::detached);
        io_context.run_one();

#undef unwrap
    };

    "constructed FileTree have same shape"_test = [&] {
        using namespace madbfs::tree;

        auto connection = mock::DummyConnection{};
        auto cache      = madbfs::data::Cache{ connection, 64 * 1024, 1024 };
        auto tree       = FileTree{ connection, cache, std::nullopt };

        auto io_context = madbfs::async::Context{};

#define unwrap(T) transform_error([](auto e) { return raise_expect_error<T>(e); }).value()

        using madbfs::path::operator""_path;

        auto coro = [&] -> madbfs::Await<void> {
            (co_await tree.mkdir("/hello"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/world.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/foo.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/movie.mp4"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/hello/bar"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/baz.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/qux.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/quux.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/bye"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/world.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/movie.mp4"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/music.mp3"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/bye/family"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/family/dad.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/family/mom.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/bob.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends/school"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/school/kal'tsit.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/school/closure.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends/work"_path, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/work/loughshinny <3.txt"_path, 0, 0)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/work/eblana?.mp4"_path, 0, 0)).unwrap(Node*);

            tree.symlink("/bye/friends/school/hehe"_path, "/bye/friends/work").unwrap(void);
            tree.symlink("/hello/wife"_path, "/bye/friends/work/loughshinny <3.txt").unwrap(void);

            (co_await tree.mknod("/bye/theresa.txt"_path, 0, 0)).unwrap(Node*);

            auto tree_str = fmt::format("\n{}", tree.root());
            expect(expected == tree_str) << diff_str(expected, tree_str);

            (co_await tree.unlink("/hello/world.txt"_path)).unwrap(void);
            (co_await tree.unlink("/bye/music.mp3"_path)).unwrap(void);
            (co_await tree.unlink("/bye/friends/bob.txt"_path)).unwrap(void);
            (co_await tree.unlink("/bye/friends/school/hehe"_path)).unwrap(void);

            // there is no recursive delete
            Node& bar   = tree.traverse("/hello/bar"_path).unwrap(Node*);
            auto  paths = Vec<madbfs::path::PathBuf>{};

            auto dummy = bar.build_path();
            dummy.extend("dummy");

            auto entries = bar.list()->get()                                         //
                         | sv::transform([](auto& node) { return node->name(); })    //
                         | sr::to<Vec<String>>();

            for (const auto& name : entries) {
                dummy.rename(name);
                (co_await tree.unlink(dummy)).unwrap(void);
            }

            (co_await tree.rmdir("/hello/bar"_path)).unwrap(void);

            tree_str = fmt::format("\n{}", tree.root());
            expect(expected_rm == tree_str) << diff_str(expected_rm, tree_str);
        };

        madbfs::async::spawn(io_context, coro(), madbfs::async::detached);
        io_context.run_one();

#undef unwrap
    };
}
