#include <madbfs-common/util/split.hpp>

#include <madbfs/connection.hpp>
#include <madbfs/filesystem.hpp>
#include <madbfs/path.hpp>

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
            auto node_expect = tree.get(id);
            if (not node_expect) {
                return;
            }

            auto& node = node_expect->get();

            for (auto _ : sv::iota(0u, depth)) {
                fmt::format_to(ctx.out(), "    ");
            }

            auto extra = node.kind().visit(madbfs::Overload{
                [&](const node::Link& l) { return fmt::format("  ->  {}", l.target.value_or("[none]")); },
                [&](const node::Directory&) { return String{ "/" }; },
                [&](const auto&) { return String{ "" }; },
            });

            auto name = node.name() == "/" ? "" : node.name();
            fmt::format_to(ctx.out(), "- {}{}\n", name, extra);

            auto list = node.as_directory().transform([&](const node::Directory& d) { return d.children(); });
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

namespace mock
{
    using namespace madbfs;

    struct DummyTransport final : public transport::Transport
    {
        Str name() const override { return "dummy"; }

        bool running() const override { return true; }

        void stop(rpc::Status) override{};

        Await<void> start() override { co_return; };

        AExpect<rpc::Response> send(rpc::Request req) override
        {
            using namespace rpc;
            co_return req.visit(Overload{
                // clang-format off
                [] (const req::Stat&         ) -> rpc::Response { return resp::Stat         {}; },
                [] (const req::Listdir&      ) -> rpc::Response { return resp::Listdir      {}; },
                [] (const req::Readlink&     ) -> rpc::Response { return resp::Readlink     {}; },
                [] (const req::Mknod&        ) -> rpc::Response { return resp::Mknod        {}; },
                [] (const req::Mkdir&        ) -> rpc::Response { return resp::Mkdir        {}; },
                [] (const req::Unlink&       ) -> rpc::Response { return resp::Unlink       {}; },
                [] (const req::Rmdir&        ) -> rpc::Response { return resp::Rmdir        {}; },
                [] (const req::Rename&       ) -> rpc::Response { return resp::Rename       {}; },
                [] (const req::Truncate&     ) -> rpc::Response { return resp::Truncate     {}; },
                [] (const req::Utimens&      ) -> rpc::Response { return resp::Utimens      {}; },
                [] (const req::CopyFileRange&) -> rpc::Response { return resp::CopyFileRange{}; },
                [] (const req::Open&         ) -> rpc::Response { return resp::Open         {}; },
                [] (const req::Close&        ) -> rpc::Response { return resp::Close        {}; },
                [] (const req::Read&         ) -> rpc::Response { return resp::Read         {}; },
                [] (const req::Write&        ) -> rpc::Response { return resp::Write        {}; },
                [] (const req::Ping&         ) -> rpc::Response { return resp::Ping         {}; },
                // clang-format on
            });
        }

        AExpect<rpc::Response> send(rpc::Request req, Milliseconds /* timeout */) override
        {
            return send(std::move(req));
        }
    };

    const auto dummy_strategy = connection_strategy::Custom{ .create = [] {
        return std::make_unique<DummyTransport>();
    } };
}

int main()
{
    using namespace ut::literals;
    using ut::expect, ut::that;

    spdlog::set_default_logger(spdlog::null_logger_mt("madbfs-test-tree"));
    // spdlog::set_level(spdlog::level::debug);

    "constructed FileTree have same shape"_test = [&] {
        using namespace madbfs;

        auto context    = madbfs::async::Context{};
        auto guard      = madbfs::net::make_work_guard(context);
        auto thread     = std::jthread{ [&] { context.run(); } };
        auto connection = madbfs::Connection{ context, mock::dummy_strategy };
        auto fs         = Filesystem{ context, connection, std::nullopt, std::nullopt };

        using madbfs::path::operator""_path;

#define unwrap(T) transform_error([](auto e) { return raise_expect_error<T>(e); }).value()

        auto coro = [&] -> madbfs::Await<void> {
            (co_await fs.mkdir("/hello"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/world.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/foo.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/movie.mp4"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/hello/bar"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/bar/baz.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/bar/qux.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/hello/bar/quux.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/bye"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/world.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/movie.mp4"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/music.mp3"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/bye/family"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/family/dad.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/family/mom.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/bye/friends"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/friends/bob.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/bye/friends/school"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/friends/school/kal'tsit.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/friends/school/closure.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mkdir("/bye/friends/work"_path, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/friends/work/loughshinny <3.txt"_path, 0, 0)).unwrap(Id);
            (co_await fs.mknod("/bye/friends/work/eblana?.mp4"_path, 0, 0)).unwrap(Id);

            fs.symlink("/bye/friends/school/hehe"_path, "/bye/friends/work").unwrap(void);
            fs.symlink("/hello/wife"_path, "/bye/friends/work/loughshinny <3.txt").unwrap(void);

            (co_await fs.mknod("/bye/theresa.txt"_path, 0, 0)).unwrap(Id);

            auto tree_str = fmt::format("\n{}", fs.tree());
            expect(expected == tree_str) << diff_str(expected, tree_str);

            (co_await fs.unlink("/hello/world.txt"_path)).unwrap(void);
            (co_await fs.unlink("/bye/music.mp3"_path)).unwrap(void);
            (co_await fs.unlink("/bye/friends/bob.txt"_path)).unwrap(void);
            (co_await fs.unlink("/bye/friends/school/hehe"_path)).unwrap(void);

            // there is no recursive delete
            auto  bar_id = fs.traverse("/hello/bar"_path).unwrap(Id);
            auto& bar    = fs.tree().get(bar_id)->get();

            auto paths = Vec<madbfs::path::PathBuf>{};
            auto dummy = fs.tree().build_path(bar_id).value();
            dummy.extend("dummy");

            auto entries = bar.as_directory()->get().children()    //
                         | sv::values                              //
                         | sv::transform([&](Id id) { return fs.tree().get(id)->get().name(); })
                         | sr::to<Vec<String>>();

            for (const auto& name : entries) {
                dummy.rename(name);
                (co_await fs.unlink(dummy)).unwrap(void);
            }

            (co_await fs.rmdir("/hello/bar"_path)).unwrap(void);

            tree_str = fmt::format("\n{}", fs.tree());
            expect(expected_rm == tree_str) << diff_str(expected_rm, tree_str);
        };

#undef unwrap

        madbfs::async::block(context, coro());

        guard.reset();
        context.stop();
    };
}
