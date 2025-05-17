#include "adbfsm/path/path.hpp"
#include "adbfsm/tree/file_tree.hpp"
#include "adbfsm/tree/node.hpp"
#include "adbfsm/util/overload.hpp"

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

            auto visitor = adbfsm::util::Overload{
                [&](const Link& link) {
                    auto pathbuf = link.target().build_path();
                    return fmt::format("    ->    {}", pathbuf.as_path().fullpath());
                },
                [&](const Directory&) { return String{ "/" }; },
                [&](const auto&) { return String{ "" }; },
            };
            auto additional = std::visit(visitor, node->value());

            // if root, don't print the name since additional will print dir mark (/)
            auto name = node->name() == "/" ? "" : node->name();
            fmt::format_to(ctx.out(), "- {}{}\n", name, additional);

            if (auto* dir = std::get_if<Directory>(&node->value())) {
                for (const auto& child : dir->children()) {
                    self(child.get(), depth + 1);
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
adbfsm::Unit raise_expect_error(std::errc errc, std::source_location loc = std::source_location::current())
{
    throw ExpectError{ errc, loc };
    return {};
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

    class DummyConnection final : public IConnection
    {
    public:
        AExpect<Gen<ParsedStat>> statdir(path::Path) override { co_return adbfsm::Unexpect{ {} }; }
        AExpect<Stat>            stat(path::Path) override { co_return Stat{}; }
        AExpect<path::PathBuf>   readlink(path::Path path) override { co_return path.into_buf(); };

        // directory operations
        AExpect<void> mkdir(path::Path) override { co_return Expect<void>{}; }
        AExpect<void> rm(path::Path, bool) override { co_return Expect<void>{}; }
        AExpect<void> rmdir(path::Path) override { co_return Expect<void>{}; }
        AExpect<void> mv(path::Path, path::Path) override { co_return Expect<void>{}; }

        // file operations
        AExpect<void>  truncate(path::Path, off_t) override { co_return Expect<void>{}; }
        AExpect<u64>   open(path::Path, int) override { co_return Expect<u64>{}; }
        AExpect<usize> read(path::Path, Span<char>, off_t) override { co_return Expect<usize>{}; }
        AExpect<usize> write(path::Path, Span<const char>, off_t) override { co_return Expect<usize>{}; }
        AExpect<void>  flush(path::Path) override { co_return Expect<void>{}; }
        AExpect<void>  release(path::Path) override { co_return Expect<void>{}; }

        // directory operation (adding file) or file operation (update time)
        AExpect<void> touch(path::Path, bool) override { co_return Expect<void>{}; }
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

#define unwrap() transform_error([](auto e) { return raise_expect_error<Node*>(e); }).value()

        using adbfsm::path::operator""_path;

        auto connection = mock::DummyConnection{};
        auto cache      = adbfsm::data::Cache{ 64 * 1024, 1024 };
        auto context    = Node::Context{ connection, cache, "/dummy"_path };

        auto io_context = adbfsm::async::Context{};

        auto coro = [&] -> adbfsm::Await<void> {
            auto root = Node{ "/", nullptr, {}, Directory{} };

            Node& hello = (co_await root.mkdir(context, "hello")).unwrap();

            _ = (co_await hello.touch(context, "world.txt")).unwrap();
            _ = (co_await hello.touch(context, "foo.txt")).unwrap();
            _ = (co_await hello.touch(context, "movie.mp4")).unwrap();

            Node& bar = (co_await hello.mkdir(context, "bar")).unwrap();

            _ = (co_await bar.touch(context, "baz.txt")).unwrap();
            _ = (co_await bar.touch(context, "qux.txt")).unwrap();
            _ = (co_await bar.touch(context, "quux.txt")).unwrap();

            Node& bye = (co_await root.mkdir(context, "bye")).unwrap();

            _ = (co_await bye.touch(context, "world.txt")).unwrap();
            _ = (co_await bye.touch(context, "movie.mp4")).unwrap();
            _ = (co_await bye.touch(context, "music.mp3")).unwrap();

            Node& family = (co_await bye.mkdir(context, "family")).unwrap();

            _ = (co_await family.touch(context, "dad.txt")).unwrap();
            _ = (co_await family.touch(context, "mom.txt")).unwrap();

            Node& friends = (co_await bye.mkdir(context, "friends")).unwrap();

            _ = (co_await friends.touch(context, "bob.txt")).unwrap();

            Node& school = (co_await friends.mkdir(context, "school")).unwrap();

            _ = (co_await school.touch(context, "kal'tsit.txt")).unwrap();
            _ = (co_await school.touch(context, "closure.txt")).unwrap();

            Node& work = (co_await friends.mkdir(context, "work")).unwrap();

            Node& wife = (co_await work.touch(context, "loughshinny <3.txt")).unwrap();

            _ = (co_await work.touch(context, "eblana?.mp4")).unwrap();
            _ = school.link("hehe", &work).unwrap();
            _ = hello.link("wife", &wife).unwrap();
            _ = (co_await bye.touch(context, "theresa.txt")).unwrap();

            auto tree_str = fmt::format("\n{}", root);
            expect(expected == tree_str) << diff_str(expected, tree_str);
        };

        adbfsm::async::spawn(io_context, coro(), adbfsm::async::detached);
        io_context.run_one();

#undef unwrap
    };

    "constructed FileTree have same shape"_test = [&] {
        using namespace adbfsm::tree;

        auto connection = mock::DummyConnection{};
        auto cache      = adbfsm::data::Cache{ 64 * 1024, 1024 };
        auto tree       = FileTree{ connection, cache };

        auto io_context = adbfsm::async::Context{};

#define unwrap(T) transform_error([](auto e) { return raise_expect_error<T>(e); }).value()

        using adbfsm::path::operator""_path;

        auto coro = [&] -> adbfsm::Await<void> {
            (co_await tree.mkdir("/hello"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/world.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/foo.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/movie.mp4"_path)).unwrap(Node*);
            (co_await tree.mkdir("/hello/bar"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/baz.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/qux.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/hello/bar/quux.txt"_path)).unwrap(Node*);
            (co_await tree.mkdir("/bye"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/world.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/movie.mp4"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/music.mp3"_path)).unwrap(Node*);
            (co_await tree.mkdir("/bye/family"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/family/dad.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/family/mom.txt"_path)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/bob.txt"_path)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends/school"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/school/kal'tsit.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/school/closure.txt"_path)).unwrap(Node*);
            (co_await tree.mkdir("/bye/friends/work"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/work/loughshinny <3.txt"_path)).unwrap(Node*);
            (co_await tree.mknod("/bye/friends/work/eblana?.mp4"_path)).unwrap(Node*);

            tree.symlink("/bye/friends/school/hehe"_path, "/bye/friends/work"_path).unwrap(void);
            tree.symlink("/hello/wife"_path, "/bye/friends/work/loughshinny <3.txt"_path).unwrap(void);

            (co_await tree.mknod("/bye/theresa.txt"_path)).unwrap(Node*);

            auto tree_str = fmt::format("\n{}", tree.root());
            expect(expected == tree_str) << diff_str(expected, tree_str);

            (co_await tree.unlink("/hello/world.txt"_path)).unwrap(void);
            (co_await tree.unlink("/bye/music.mp3"_path)).unwrap(void);
            (co_await tree.unlink("/bye/friends/bob.txt"_path)).unwrap(void);
            (co_await tree.unlink("/bye/friends/school/hehe"_path)).unwrap(void);

            // there is no recursive delete
            Node& bar      = tree.traverse("/hello/bar"_path).unwrap(Node*);
            auto  bar_path = bar.build_path();
            auto  paths    = std::vector<adbfsm::path::PathBuf>{};

            _ = bar.list([&](Str name) { paths.push_back(bar_path.extend_copy(name).value()); });
            for (const auto& path : paths) {
                (co_await tree.unlink(path.as_path())).unwrap(void);
            }

            (co_await tree.rmdir("/hello/bar"_path)).unwrap(void);

            tree_str = fmt::format("\n{}", tree.root());
            expect(expected_rm == tree_str) << diff_str(expected_rm, tree_str);
        };

        adbfsm::async::spawn(io_context, coro(), adbfsm::async::detached);
        io_context.run_one();

#undef unwrap
    };
}
