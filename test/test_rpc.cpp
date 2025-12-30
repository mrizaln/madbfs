#include "util.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

#include <boost/ut.hpp>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <print>
#include <random>
#include <thread>

namespace ut    = boost::ut;
namespace rpc   = madbfs::rpc;
namespace async = madbfs::async;
namespace net   = madbfs::net;

using madbfs::Await, madbfs::AExpect;
using namespace madbfs::aliases;

static constexpr u16 port = 54321;

Await<rpc::Socket> connect()
{
    auto exec   = co_await async::current_executor();
    auto socket = async::tcp::Socket{ exec };

    auto address  = net::ip::address_v4{ { 127, 0, 0, 1 } };    // localhost
    auto endpoint = net::ip::tcp::endpoint{ address, port };

    if (auto res = co_await socket.async_connect(endpoint); not res) {
        throw boost::system::system_error{ res.error() };
    }

    co_return socket;
}

Await<Var<rpc::Status, rpc::Response>> server_handler(Vec<u8>&, rpc::Request request)
{
    static_assert(std::is_standard_layout_v<rpc::Request>);

    using rpc::req::Read, rpc::req::Write, rpc::req::Close;

    co_return request.visit([]<rpc::IsRequest R>(R r) -> Var<rpc::Status, rpc::Response> {
        // to test whether the server received the correct type of the request  I put the stringized name of
        // the type into the first member of variants that takes path as its parameter otherwise if the first
        // parameter is file descriptor (u64) it will be set to its variant index.

        // https://stackoverflow.com/a/25377970.

        if constexpr (std::same_as<Read, R> or std::same_as<Write, R> or std::same_as<Close, R>) {
            auto index = reinterpret_cast<u64*>(&r);
            if (*index != rpc::Request::index_of<R>()) {
                return rpc::Status::invalid_argument;
            }
        } else {
            auto name = reinterpret_cast<Str*>(&r);
            if (*name != ut::reflection::type_name<R>()) {
                return rpc::Status::invalid_argument;
            }
        }
        return rpc::ToResp<R>{};
    });
}

Await<void> acceptor_loop(async::Context& context)
{
    auto acceptor = async::tcp::Acceptor{ context, { async::tcp::Proto::v4(), port } };

    acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
    acceptor.listen();

    while (true) {
        auto sock = co_await acceptor.async_accept();
        if (not sock) {
            break;
        }

        auto server = rpc::Server{ std::move(sock).value() };
        std::ignore = co_await server.listen(server_handler);
    }
}

template <rpc::IsRequest R>
rpc::Request create_req()
{
    using rpc::req::Read, rpc::req::Write, rpc::req::Close;

    // for requests that take path as its parameter, the generated request will set the path to its type name
    // otherwise if the first parameter is file descriptor (u64) it will be set to its variant index

    // mental gymnastics to bypass the missing initializer warning :P
    // cast to first field: https://stackoverflow.com/a/25377970.
    auto r = R{};

    if constexpr (std::same_as<Read, R> or std::same_as<Write, R> or std::same_as<Close, R>) {
        auto index = reinterpret_cast<u64*>(&r);
        *index     = rpc::Request::index_of<R>();
    } else {
        auto name = reinterpret_cast<Str*>(&r);
        *name     = ut::reflection::type_name<R>();
    }

    return r;
};

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::throws, ut::log, ut::that, ut::fatal;

    spdlog::set_default_logger(spdlog::null_logger_mt("madbfs-test-rpc"));

    auto context = async::Context{};
    auto guard   = net::make_work_guard(context);
    auto thread  = std::jthread{ [&] { context.run(); } };

    async::spawn(context, acceptor_loop(context), async::detached);

    auto sync_wait = [&context]<typename T>(Await<T> coro) -> T {
        return async::block(context, std::move(coro));
    };

    "rpc client-server communication should still work even in high traffic"_test = [&] {
        constexpr auto multiplier   = 200uz;
        constexpr auto variants_num = std::variant_size_v<rpc::Request::Var>;

        auto socket = sync_wait(connect());
        auto client = rpc::Client{ std::move(socket) };

        async::spawn(context, client.start(), async::detached);

        auto buffers    = Vec<Vec<u8>>{};
        auto coroutines = Vec<AExpect<rpc::Response>>{};
        auto indices    = Vec<usize>{};

        buffers.resize(variants_num * multiplier);

        for (auto i : sv::iota(0uz, multiplier)) {
            test::util::for_each_variant<rpc::Request::Var>([&]<usize I, rpc::IsRequest R>() {
                auto& buffer = buffers[i * variants_num + I];
                coroutines.emplace_back(client.send_req(buffer, create_req<R>(), {}));
                indices.emplace_back(I);
            });
        }

        auto rng = std::mt19937{ std::random_device{}() };
        test::util::shuffle(rng, coroutines, indices);

        namespace chr = std::chrono;

        auto start     = chr::steady_clock::now();
        auto responses = sync_wait(async::wait_all(std::move(coroutines)));
        auto duration  = chr::steady_clock::now() - start;

        auto to_sec = [](auto dur) { return chr::duration_cast<chr::duration<float>>(dur); };
        auto to_ms  = [](auto dur) { return chr::duration_cast<chr::duration<float, std::milli>>(dur); };

        std::println("duration: {} ({} per ops)", to_sec(duration), to_ms(duration / coroutines.size()));

        for (auto i : sv::iota(0uz, variants_num * multiplier)) {
            auto&& resp  = responses[i];
            auto   index = indices[i];

            expect(resp.has_value() >> fatal);
            expect(resp->index() == index);
        }
    };

    guard.reset();
    context.stop();
}
