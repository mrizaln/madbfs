#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

#include <boost/ut.hpp>
#include <fmt/base.h>
#include <fmt/std.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <chrono>
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

    co_return request.visit([]<rpc::IsRequest R>(R r) -> Var<rpc::Status, rpc::Response> {
        // to test whether the server received the correct type of the request  I put the stringized name of
        // the type into the first member of the variants. the first member of the Request variants are always
        // Str, so that should be feasible and retrieving the value is allowed:
        // https://stackoverflow.com/a/25377970.

        auto name = reinterpret_cast<Str*>(&r);

        if (*name != ut::reflection::type_name<R>()) {
            return rpc::Status::invalid_argument;
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
    // mental gymnastics to bypass the missing initializer warning :P
    auto r = R{};

    // first field always Str, so this is allowed: https://stackoverflow.com/a/25377970.
    auto name = reinterpret_cast<Str*>(&r);
    *name     = ut::reflection::type_name<R>();

    return r;
};

template <typename Var, typename Fn>
inline constexpr auto for_each_variant(Fn&& fn)
{
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (fn.template operator()<I, std::variant_alternative_t<I, Var>>(), ...);
    }(std::make_index_sequence<std::variant_size_v<std::decay_t<Var>>>());
}

template <std::ranges::contiguous_range... Rs>
static void shuffle(std::mt19937& rng, Rs&... rs)
{
    using Dist  = std::uniform_int_distribution<usize>;
    using Param = typename Dist::param_type;

    auto dist = Dist{};
    auto n    = std::min({ std::ranges::size(rs)... });

    for (auto i = usize{ n - 1 }; i > 0; --i) {
        auto j = dist(rng, Param{ 0, i });
        (std::swap(rs[i], rs[j]), ...);
    }
}

int main()
{
    spdlog::set_default_logger(spdlog::null_logger_mt("madbfs-test-rpc"));

    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::throws, ut::log, ut::that, ut::fatal;

    auto context = async::Context{};
    auto guard   = net::make_work_guard(context);
    auto thread  = std::jthread{ [&] { context.run(); } };

    async::spawn(context, acceptor_loop(context), async::detached);

    auto sync_wait = [&context]<typename T>(Await<T> coro) -> T {
        return async::block(context, std::move(coro));
    };

    "test client-server communication even in high traffic"_test = [&] {
        constexpr auto multiplier   = 1000uz;
        constexpr auto variants_num = std::variant_size_v<rpc::Request::Var>;

        auto socket = sync_wait(connect());
        auto client = rpc::Client{ std::move(socket) };

        async::spawn(context, client.start(), async::detached);

        auto buffers    = Vec<Vec<u8>>{};
        auto coroutines = Vec<AExpect<rpc::Response>>{};
        auto indices    = Vec<usize>{};

        buffers.resize(variants_num * multiplier);

        for (auto i : sv::iota(0uz, multiplier)) {
            for_each_variant<rpc::Request::Var>([&]<usize I, rpc::IsRequest R>() {
                auto& buffer = buffers[i * variants_num + I];
                coroutines.emplace_back(client.send_req(buffer, create_req<R>(), {}));
                indices.emplace_back(I);
            });
        }

        auto rng = std::mt19937{ std::random_device{}() };
        shuffle(rng, coroutines, indices);

        namespace chr = std::chrono;

        auto start     = chr::steady_clock::now();
        auto responses = sync_wait(async::wait_all(std::move(coroutines)));
        auto duration  = chr::steady_clock::now() - start;

        auto to_sec = [](auto dur) { return chr::duration_cast<chr::duration<float>>(dur); };
        auto to_ms  = [](auto dur) { return chr::duration_cast<chr::duration<float, std::milli>>(dur); };

        fmt::println("duration: {} ({} per ops)", to_sec(duration), to_ms(duration / coroutines.size()));

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
