#include "madbfs/connection.hpp"

#include "madbfs/cmd.hpp"
#include "madbfs/path.hpp"
#include "madbfs/transport/adb_transport.hpp"
#include "madbfs/transport/null_transport.hpp"
#include "madbfs/transport/proxy_transport.hpp"

#include <limits>
#include <madbfs-common/log.hpp>
#include <madbfs-common/util/split.hpp>

#include <random>

namespace
{
    madbfs::Await<madbfs::Uniq<madbfs::transport::Transport>> create_transport(
        const madbfs::ConnectionStrategy& strat
    )
    {
        using namespace madbfs;
        namespace conn = connection_strategy;

        switch (strat.index()) {
        case ConnectionStrategy::index_of<conn::Custom>(): {
            auto custom = strat.as<conn::Custom>();
            co_return custom->create();
        }
        case ConnectionStrategy::index_of<conn::Proxy>(): {
            auto proxy     = strat.as<conn::Proxy>();
            auto transport = co_await transport::ProxyTransport::create(proxy->server, proxy->port);
            if (transport) {
                co_return std::move(*transport);
            }
            [[fallthrough]];
        }
        case ConnectionStrategy::index_of<conn::Adb>(): {
            auto exec = co_await async::current_executor();
            co_return std::make_unique<transport::AdbTransport>(exec);
        }
        case ConnectionStrategy::index_of<conn::Null>(): [[fallthrough]];
        default: co_return std::make_unique<transport::NullTransport>(Errc::not_connected);
        }
    }

    madbfs::AExpect<madbfs::Uniq<madbfs::transport::Transport>> create_optimized_transport(
        const madbfs::ConnectionStrategy& strat
    )
    {
        using namespace madbfs;
        namespace conn = connection_strategy;

        switch (strat.index()) {
        case ConnectionStrategy::index_of<conn::Custom>(): {
            auto custom = strat.as<conn::Custom>();
            co_return custom->create();
        }
        case ConnectionStrategy::index_of<conn::Proxy>(): {
            auto proxy = strat.as<conn::Proxy>();
            co_return co_await transport::ProxyTransport::create(proxy->server, proxy->port);
        }
        case ConnectionStrategy::index_of<conn::Adb>(): {
            auto exec = co_await async::current_executor();
            co_return std::make_unique<transport::AdbTransport>(exec);
        }
        case ConnectionStrategy::index_of<conn::Null>(): [[fallthrough]];
        default: co_return std::make_unique<transport::NullTransport>(Errc::not_connected);
        }
    }

    madbfs::Str strategy_name(const madbfs::ConnectionStrategy& strat)
    {
        namespace conn = madbfs::connection_strategy;
        return strat.visit(madbfs::Overload{
            [](const conn::Proxy&) { return "proxy"; },
            [](const conn::Adb&) { return "adb"; },
            [](const conn::Null&) { return "null"; },
            [](const conn::Custom&) { return "custom"; },
        });
    }
}

namespace madbfs
{
    Connection::~Connection() = default;

    Connection::Connection(async::Context& ctx, ConnectionStrategy strat)
        : m_transport{ nullptr }
        , m_strategy{ strat }
    {
        log_i("{}: Connection is constructed using this strategy: {}", __func__, strategy_name(strat));
        m_transport = async::block(ctx, create_transport(strat));
    }

    Await<void> Connection::start()
    {
        co_await m_transport->start();
    }

    Str Connection::name() const
    {
        return m_transport->name();
    }

    void Connection::cancel(Errc err)
    {
        m_transport->stop(err);
    }

    bool Connection::is_optimal() const
    {
        return m_transport->name() == strategy_name(m_strategy);
    }

    AExpect<void> Connection::optimize()
    {
        if (is_optimal()) {
            co_return Unexpect{ Errc::already_connected };
        }

        auto transport = co_await create_optimized_transport(m_strategy);
        if (not transport) {
            co_return Unexpect{ transport.error() };
        } else if ((**transport).name() == m_transport->name()) {
            co_return Expect<void>{};    // well, do nothing
        }

        log_i("{}: new {} transport created", __func__, (**transport).name());

        if (m_reconnection) {
            co_return Unexpect{ Errc::operation_in_progress };
        }

        auto promise   = saf::promise<Errc>{ co_await async::current_executor() };
        m_reconnection = promise.get_future().share();

        m_transport->stop(Errc::resource_unavailable_try_again);

        const auto old = m_transport->name();
        m_transport    = std::move(*transport);

        log_i("{}: {} transport replaced with {} transport", __func__, old, m_transport->name());

        promise.set_value(Errc{});
        m_reconnection.reset();

        co_return Expect<void>{};
    }

    AExpect<void> Connection::reconnect()
    {
        if (m_reconnection) {
            co_return Unexpect{ Errc::operation_in_progress };
        }

        auto promise   = saf::promise<Errc>{ co_await async::current_executor() };
        m_reconnection = promise.get_future().share();

        m_transport->stop(Errc::resource_unavailable_try_again);

        const auto old = m_transport->name();
        m_transport    = co_await create_transport(m_strategy);

        log_i("{}: {} transport replaced with {} transport", __func__, old, m_transport->name());

        promise.set_value(Errc{});
        m_reconnection.reset();

        co_return Expect<void>{};
    }

    AExpect<void> Connection::ping(Opt<Seconds> timeout)
    {
        static auto rng  = std::mt19937{ std::random_device{}() };
        static auto dist = std::uniform_int_distribution<u64>(0, std::numeric_limits<u64>::max());

        auto buf = Vec<u8>{};
        auto num = dist(rng);

        // special for ping only, it won't bother with reconnection
        auto res = timeout ? co_await m_transport->send_req(buf, rpc::req::Ping{ .num = num }, *timeout)
                           : co_await m_transport->send_req(buf, rpc::req::Ping{ .num = num });

        co_return res.and_then([=](rpc::resp::Ping resp) -> Expect<void> {
            return num != resp.num ? Unexpect{ Errc::invalid_argument } : Expect<void>{};
        });
    }

    AExpect<Gen<ParsedStat>> Connection::statdir(path::Path path)
    {
        auto buf  = Vec<u8>{};
        auto req  = rpc::req::Listdir{ .path = path };
        auto resp = co_await send_req_with_reconnection(buf, req);
        if (not resp) {
            co_return Unexpect{ resp.error() };
        }

        auto generator = [](Vec<u8> buf, Vec<Pair<Str, rpc::resp::Stat>> entries) -> Gen<ParsedStat> {
            for (const auto& [name, stat] : entries) {
                co_yield ParsedStat{
                    .stat = data::Stat {
                        .links = stat.links,
                        .size  = stat.size,
                        .mtime = stat.mtime,
                        .atime = stat.atime,
                        .ctime = stat.ctime,
                        .mode  = stat.mode,
                        .uid   = stat.uid,
                        .gid   = stat.gid,
                    },
                    .name = name,       // names are stored in buf
                };
            }
        };

        co_return generator(std::move(buf), std::move(resp).value().entries);
    }

    AExpect<data::Stat> Connection::stat(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Stat{ .path = path };

        co_return (co_await send_req_with_reconnection(buf, req)).transform([](rpc::resp::Stat resp) {
            return data::Stat{
                .links = resp.links,
                .size  = resp.size,
                .mtime = resp.mtime,
                .atime = resp.atime,
                .ctime = resp.ctime,
                .mode  = resp.mode,
                .uid   = resp.uid,
                .gid   = resp.gid,
            };
        });
    }

    AExpect<String> Connection::readlink(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Readlink{ .path = path };

        co_return (co_await send_req_with_reconnection(buf, req)).transform([&](rpc::resp::Readlink resp) {
            return String{ resp.target };
        });
    }

    AExpect<void> Connection::mknod(path::Path path, mode_t mode, dev_t dev)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Mknod{ .path = path, .mode = mode, .dev = dev };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::mkdir(path::Path path, mode_t mode)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Mkdir{ .path = path, .mode = mode };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::unlink(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Unlink{ .path = path };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::rmdir(path::Path path)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Rmdir{ .path = path };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::rename(path::Path from, path::Path to, u32 flags)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Rename{ .from = from, .to = to, .flags = flags };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::truncate(path::Path path, off_t size)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Truncate{ .path = path, .size = size };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<void> Connection::utimens(path::Path path, timespec atime, timespec mtime)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Utimens{ .path = path, .atime = atime, .mtime = mtime };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<usize> Connection::copy_file_range(
        path::Path in,
        off_t      in_off,
        path::Path out,
        off_t      out_off,
        usize      size
    )
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::CopyFileRange{
            .in_path    = in,
            .in_offset  = in_off,
            .out_path   = out,
            .out_offset = out_off,
            .size       = size,
        };

        co_return (co_await send_req_with_reconnection(buf, req))
            .transform(proj(&rpc::resp::CopyFileRange::size));
    }

    AExpect<u64> Connection::open(path::Path path, data::OpenMode mode)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Open{ .path = path, .mode = static_cast<rpc::OpenMode>(mode) };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(proj(&rpc::resp::Open::fd));
    }

    AExpect<void> Connection::close(u64 fd)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Close{ .fd = fd };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(sink_void);
    }

    AExpect<usize> Connection::read(u64 fd, Span<char> out, off_t offset)
    {
        auto buf = Vec<u8>{};
        auto req = rpc::req::Read{ .fd = fd, .offset = offset, .size = out.size() };

        co_return (co_await send_req_with_reconnection(buf, req)).transform([&](rpc::resp::Read resp) {
            auto size = std::min(resp.read.size(), out.size());
            std::copy_n(resp.read.begin(), size, out.begin());
            return size;
        });
    }

    AExpect<usize> Connection::write(u64 fd, Span<const char> in, off_t offset)
    {
        auto buf   = Vec<u8>{};
        auto bytes = Span{ reinterpret_cast<const u8*>(in.data()), in.size() };
        auto req   = rpc::req::Write{ .fd = fd, .offset = offset, .in = bytes };

        co_return (co_await send_req_with_reconnection(buf, req)).transform(proj(&rpc::resp::Write::size));
    }

    Await<Opt<Errc>> Connection::check_reconnection()
    {
        if (m_reconnection) {
            auto future = *m_reconnection;
            co_await future.async_wait();
            co_return future.get();
        }

        co_return std::nullopt;
    }
}

namespace madbfs
{
    Str to_string(DeviceStatus status)
    {
        switch (status) {
        case DeviceStatus::Device: return "device";
        case DeviceStatus::Emulator: return "emulator";
        case DeviceStatus::Offline: return "offline";
        case DeviceStatus::Unauthorized: return "unauthorized";
        case DeviceStatus::Unknown: return "unknown";
        }
        return "Unknown";
    }

    AExpect<void> start_adb_server()
    {
        auto res = co_await cmd::exec({ "adb", "start-server" });
        co_return res.transform(sink_void);
    }

    AExpect<Vec<Device>> list_adb_devices()
    {
        auto res = co_await cmd::exec({ "adb", "devices" });

        if (not res.has_value()) {
            co_return Unexpect{ res.error() };
        }

        auto devices = Vec<Device>{};

        auto line_splitter = util::StringSplitter{ *res, { '\n' } };
        std::ignore        = line_splitter.next();    // skip the first line

        while (auto str = line_splitter.next()) {
            auto splitter = util::StringSplitter{ *str, { " \t" } };

            auto serial_str = splitter.next();
            auto status_str = splitter.next();

            if (not serial_str.has_value() or not status_str.has_value()) {
                continue;
            }

            auto status = DeviceStatus::Unknown;

            // clang-format off
            if      (*status_str == "offline")      status = DeviceStatus::Offline;
            else if (*status_str == "unauthorized") status = DeviceStatus::Unauthorized;
            else if (*status_str == "emulator")     status = DeviceStatus::Emulator;
            else if (*status_str == "device")       status = DeviceStatus::Device;
            // clang-format on

            devices.emplace_back(String{ *serial_str }, status);
        }

        co_return devices;
    }
}
