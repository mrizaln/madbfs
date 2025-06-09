#pragma once

#include "madbfs/path/path.hpp"

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>

#include <boost/json.hpp>

#include <functional>

namespace madbfs::data
{
    namespace ipc
    {
        // clang-format off
        struct Help            { };
        struct InvalidateCache { };
        struct SetPageSize     { usize kib; };
        struct GetPageSize     { };
        struct SetCacheSize    { usize mib; };
        struct GetCacheSize    { };
        // clang-format on

        using Op = Var<Help, InvalidateCache, SetPageSize, GetPageSize, SetCacheSize, GetCacheSize>;
    }

    class Ipc
    {
    public:
        using Acceptor = async::unix_socket::Acceptor;
        using Socket   = async::unix_socket::Socket;
        using OnOp     = std::move_only_function<Await<boost::json::value>(ipc::Op op)>;

        // Uniq to make it std::movable
        static Expect<Uniq<Ipc>> create(async::Context& context);

        ~Ipc();

        Ipc(Ipc&&)            = default;
        Ipc& operator=(Ipc&&) = default;

        Ipc(const Ipc&)            = delete;
        Ipc& operator=(const Ipc&) = delete;

        Await<void> launch(OnOp on_op);
        void        stop();

        path::Path path() const { return m_socket_path.as_path(); }

    private:
        Ipc(path::PathBuf path, Acceptor acceptor)
            : m_socket_path{ std::move(path) }
            , m_socket{ std::move(acceptor) }
        {
        }

        Await<void> run();
        Await<void> handle_peer(Socket sock);

        path::PathBuf m_socket_path;
        Acceptor      m_socket;
        OnOp          m_on_op;
        bool          m_running = false;    // should have been an atomic
    };
}
