#include "util.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/ipc.hpp>
#include <madbfs-common/util/defer.hpp>

#include <boost/json.hpp>
#include <boost/ut.hpp>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <print>

namespace fs    = std::filesystem;
namespace ut    = boost::ut;
namespace ipc   = madbfs::ipc;
namespace async = madbfs::async;
namespace net   = madbfs::net;

using madbfs::Await, madbfs::AExpect;
using namespace madbfs::aliases;

Await<boost::json::value> server_handler(ipc::FsOp op)
{
    co_return boost::json::value{
        { "type", op.visit([]<typename Op>(Op) { return ut::reflection::type_name<Op>(); }) }
    };
}

template <typename Var>
constexpr Opt<Str> variant_name_from_index(usize index)
{
    auto name = Opt<Str>{};
    test::util::for_each_variant<Var>([&]<usize I, typename T>() {
        if (index == I) {
            name = ut::reflection::type_name<T>();
        }
    });
    return name;
}

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::throws, ut::log, ut::that, ut::fatal;

    spdlog::set_default_logger(spdlog::null_logger_mt("madbfs-test-ipc"));
    // spdlog::set_level(spdlog::level::debug);

    auto context = async::Context{};
    auto guard   = net::make_work_guard(context);
    auto thread  = std::jthread{ [&] { context.run(); } };

    auto sync_wait = [&context]<typename T>(Await<T> coro) -> T {
        return async::block(context, std::move(coro));
    };

    auto socket_path = "/tmp/madbfs-test-ipc.sock";
    auto deferred    = madbfs::util::defer([&] { fs::remove(socket_path); });

    // in case the file is not removed on the last test
    fs::remove(socket_path);

    auto server = ipc::Server::create(context, socket_path);
    async::spawn(context, server->launch(server_handler), async::detached);

    "ipc client-server communication should still work even in high traffic"_test = [&] {
        constexpr auto multiplier   = 200uz;
        constexpr auto variants_num = std::variant_size_v<ipc::FsOp::Var>;

        auto ops     = Vec<ipc::FsOp>{};
        auto indices = Vec<usize>{};

        for (auto _ : sv::iota(0uz, multiplier)) {
            test::util::for_each_variant<ipc::FsOp::Var>([&]<usize I, typename Op>() {
                if constexpr (std::same_as<Op, ipc::op::SetLogLevel>) {
                    ops.emplace_back(Op{ .lvl = "debug" });
                } else {
                    ops.emplace_back(Op{});
                }
                indices.emplace_back(I);
            });
        }

        auto rng = std::mt19937{ std::random_device{}() };
        test::util::shuffle(rng, ops, indices);

        namespace chr = std::chrono;

        auto start = chr::steady_clock::now();

        for (auto i : sv::iota(0uz, variants_num * multiplier)) {
            auto op    = ops[i];
            auto index = indices[i];

            auto client = ipc::Client::create(context, socket_path);
            expect(client.has_value() >> fatal);

            auto resp = sync_wait(client->send(op));
            expect(resp.has_value() >> fatal);

            auto resp_str = boost::json::serialize(*resp);
            auto expected = boost::json::serialize(boost::json::value{
                { "status", "success" },
                { "value", { { "type", *variant_name_from_index<ipc::FsOp::Var>(index) } } },
            });

            expect(resp_str == expected);
        }

        auto duration = chr::steady_clock::now() - start;

        auto to_sec = [](auto dur) { return chr::duration_cast<chr::duration<float>>(dur); };
        auto to_ms  = [](auto dur) { return chr::duration_cast<chr::duration<float, std::milli>>(dur); };

        std::println("duration: {} ({} per ops)", to_sec(duration), to_ms(duration / ops.size()));
    };

    guard.reset();
    context.stop();
}
