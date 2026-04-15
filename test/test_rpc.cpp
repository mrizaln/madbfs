#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/log.hpp>
#include <madbfs-common/rpc.hpp>

#include <boost/ut.hpp>
#include <spdlog/spdlog.h>

#include <thread>

namespace ut    = boost::ut;
namespace rpc   = madbfs::rpc;
namespace async = madbfs::async;
namespace net   = madbfs::net;

using madbfs::AExpect;
using madbfs::Await;
using namespace madbfs::aliases;

using madbfs::err_msg;
using madbfs::log_e;

static constexpr u16 echo_request_port  = 54321;
static constexpr u16 echo_response_port = 54322;

Await<rpc::Socket> connect(u16 port)
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

Await<void> echo_request()
{
    auto exec     = co_await async::current_executor();
    auto acceptor = async::tcp::Acceptor{ exec, { async::tcp::Proto::v4(), echo_request_port } };

    acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
    acceptor.listen();

    while (true) {
        auto sock = co_await acceptor.async_accept();
        if (not sock) {
            log_e("{}: failed to accept connection: {}", __func__, sock.error().message());
            break;
        }

        auto header = co_await rpc::receive_request_header(*sock);
        if (not header) {
            log_e("{}: failed to read request header: {}", __func__, err_msg(header.error()));
            continue;
        }

        auto req_buf = Vec<u8>{};
        auto req     = co_await rpc::receive_request(*sock, req_buf, *header);
        if (not req) {
            log_e("{}: failed to receive request: {}", __func__, err_msg(req.error()));
            continue;
        }

        auto payload_buf = Vec<u8>{};
        std::ignore      = co_await rpc::send_request(*sock, payload_buf, *req, header->id);
    }
}

Await<void> echo_response()
{
    auto exec     = co_await async::current_executor();
    auto acceptor = async::tcp::Acceptor{ exec, { async::tcp::Proto::v4(), echo_response_port } };

    acceptor.set_option(async::tcp::Acceptor::reuse_address(true));
    acceptor.listen();

    while (true) {
        auto sock = co_await acceptor.async_accept();
        if (not sock) {
            log_e("{}: failed to accept connection: {}", __func__, sock.error().message());
            break;
        }

        auto header = co_await rpc::receive_response_header(*sock);
        if (not header) {
            log_e("{}: failed to read response header: {}", __func__, err_msg(header.error()));
            continue;
        }

        auto req_buf = Vec<u8>{};
        auto req     = co_await rpc::receive_response(*sock, req_buf, *header);
        if (not req) {
            log_e("{}: failed to receive response: {}", __func__, err_msg(req.error()));
            continue;
        }

        auto payload_buf = Vec<u8>{};
        std::ignore      = co_await rpc::send_response(*sock, payload_buf, req->proc(), *req, header->id);
    }
}

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;

    // spdlog::set_level(spdlog::level::debug);

    auto context = async::Context{};
    auto guard   = net::make_work_guard(context);
    auto thread  = std::jthread{ [&] { context.run(); } };

    async::spawn(context, echo_request(), async::detached);
    async::spawn(context, echo_response(), async::detached);

    "Request and Responses variants procedure value should corresponds with each other"_test = [&] {
        using namespace rpc;

        // clang-format off
        ut::expect(Request{ req::Stat         {} }.proc() == Procedure::Stat         );
        ut::expect(Request{ req::Listdir      {} }.proc() == Procedure::Listdir      );
        ut::expect(Request{ req::Readlink     {} }.proc() == Procedure::Readlink     );
        ut::expect(Request{ req::Mknod        {} }.proc() == Procedure::Mknod        );
        ut::expect(Request{ req::Mkdir        {} }.proc() == Procedure::Mkdir        );
        ut::expect(Request{ req::Unlink       {} }.proc() == Procedure::Unlink       );
        ut::expect(Request{ req::Rmdir        {} }.proc() == Procedure::Rmdir        );
        ut::expect(Request{ req::Rename       {} }.proc() == Procedure::Rename       );
        ut::expect(Request{ req::Truncate     {} }.proc() == Procedure::Truncate     );
        ut::expect(Request{ req::Utimens      {} }.proc() == Procedure::Utimens      );
        ut::expect(Request{ req::CopyFileRange{} }.proc() == Procedure::CopyFileRange);
        ut::expect(Request{ req::Open         {} }.proc() == Procedure::Open         );
        ut::expect(Request{ req::Close        {} }.proc() == Procedure::Close        );
        ut::expect(Request{ req::Read         {} }.proc() == Procedure::Read         );
        ut::expect(Request{ req::Write        {} }.proc() == Procedure::Write        );
        ut::expect(Request{ req::Ping         {} }.proc() == Procedure::Ping         );
        // clang-format on

        // clang-format off
        ut::expect(Response{ resp::Stat         {} }.proc() == Procedure::Stat         );
        ut::expect(Response{ resp::Listdir      {} }.proc() == Procedure::Listdir      );
        ut::expect(Response{ resp::Readlink     {} }.proc() == Procedure::Readlink     );
        ut::expect(Response{ resp::Mknod        {} }.proc() == Procedure::Mknod        );
        ut::expect(Response{ resp::Mkdir        {} }.proc() == Procedure::Mkdir        );
        ut::expect(Response{ resp::Unlink       {} }.proc() == Procedure::Unlink       );
        ut::expect(Response{ resp::Rmdir        {} }.proc() == Procedure::Rmdir        );
        ut::expect(Response{ resp::Rename       {} }.proc() == Procedure::Rename       );
        ut::expect(Response{ resp::Truncate     {} }.proc() == Procedure::Truncate     );
        ut::expect(Response{ resp::Utimens      {} }.proc() == Procedure::Utimens      );
        ut::expect(Response{ resp::CopyFileRange{} }.proc() == Procedure::CopyFileRange);
        ut::expect(Response{ resp::Open         {} }.proc() == Procedure::Open         );
        ut::expect(Response{ resp::Close        {} }.proc() == Procedure::Close        );
        ut::expect(Response{ resp::Read         {} }.proc() == Procedure::Read         );
        ut::expect(Response{ resp::Write        {} }.proc() == Procedure::Write        );
        ut::expect(Response{ resp::Ping         {} }.proc() == Procedure::Ping         );
        // clang-format on
    };

    "Request should survive roundtrip"_test = [&] {
        using namespace rpc;

        auto socket = async::block(context, connect(echo_request_port));

        auto id      = Id{ 42 };
        auto buffer  = Vec<u8>{};
        auto request = req::CopyFileRange{
            .in_path    = "adsf9ewafu",
            .in_offset  = 384'734'987,
            .out_path   = "4tr7fw /adsf",
            .out_offset = 239874,
            .size       = 437'493'873,
        };

        std::ignore = async::block(context, rpc::send_request(socket, buffer, request, id));

        auto header = async::block(context, rpc::receive_request_header(socket));
        ut::expect(header.has_value() >> ut::fatal);

        auto roundtrip = async::block(context, rpc::receive_request(socket, buffer, *header));
        ut::expect(roundtrip.has_value() >> ut::fatal);
        ut::expect(roundtrip->proc() == Procedure::CopyFileRange);

        auto underlying = std::get<req::CopyFileRange>(*roundtrip);

        // clang-format off
        ut::expect(request.in_path    == underlying.in_path    );
        ut::expect(request.in_offset  == underlying.in_offset  );
        ut::expect(request.out_path   == underlying.out_path   );
        ut::expect(request.out_offset == underlying.out_offset );
        ut::expect(request.size       == underlying.size       );
        // clang-format on
    };

    "Response should survive roundtrip"_test = [&] {
        using namespace rpc;

        auto socket = async::block(context, connect(echo_response_port));

        auto id     = Id{ 42 };
        auto buffer = Vec<u8>{};
        auto proc   = Procedure::Listdir;
        auto response = resp::Listdir{
            .entries = Vec<Pair<Str, resp::Stat>>{
                { "earth", resp::Stat{ 
                    .size  = 3974,
                    .links = 24,
                    .mtime = { 395'840'874 , 352'875'487},
                    .atime = { 34'986'745'876, 49903} ,
                    .ctime = { 348390, 9602},
                    .mode  = 26'500'736,
                    .uid   = 34837,
                    .gid   = 349881, } },
                { "terra", resp::Stat{ 
                    .size  = 3988,
                    .links = 32,
                    .mtime = { 401'839'881 , 349'873'487},
                    .atime = { 41'994'743'905, 49898} ,
                    .ctime = { 348397, 9460},
                    .mode  = 23'505'734,
                    .uid   = 34837,
                    .gid   = 349890, } },
                { "talos", resp::Stat{ 
                    .size  = 3973,
                    .links = 23,
                    .mtime = { 394'839'873 , 349'873'487},
                    .atime = { 34'983'743'874, 49898} ,
                    .ctime = { 348387, 9599},
                    .mode  = 23'498'734,
                    .uid   = 34834,
                    .gid   = 349874, } },
            },
        };

        std::ignore = async::block(context, rpc::send_response(socket, buffer, proc, response, id));

        auto header = async::block(context, rpc::receive_response_header(socket));
        ut::expect(header.has_value() >> ut::fatal);

        auto roundtrip = async::block(context, rpc::receive_response(socket, buffer, *header));
        ut::expect(roundtrip.has_value() >> ut::fatal);
        ut::expect(roundtrip->proc() == Procedure::Listdir);

        auto underlying = std::get<resp::Listdir>(*roundtrip);
        ut::expect(response.entries.size() == underlying.entries.size() >> ut::fatal);

        auto stat_compare = [](resp::Stat s1, resp::Stat s2) {
            return s1.size == s2.size                      //
                && s1.links == s2.links                    //
                && s1.mtime.tv_sec == s2.mtime.tv_sec      //
                && s1.atime.tv_sec == s2.atime.tv_sec      //
                && s1.ctime.tv_sec == s2.ctime.tv_sec      //
                && s1.mtime.tv_nsec == s2.mtime.tv_nsec    //
                && s1.atime.tv_nsec == s2.atime.tv_nsec    //
                && s1.ctime.tv_nsec == s2.ctime.tv_nsec    //
                && s1.mode == s2.mode                      //
                && s1.uid == s2.uid                        //
                && s1.gid == s2.gid;
        };

        for (auto i : sv::iota(0uz, underlying.entries.size())) {
            ut::expect(response.entries[i].first == underlying.entries[i].first);
            ut::expect(stat_compare(response.entries[i].second, underlying.entries[i].second) == true);
        }
    };

    guard.reset();
    context.stop();
}
