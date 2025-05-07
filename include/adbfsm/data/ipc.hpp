#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/path/path.hpp"
#include "adbfsm/util/threadpool.hpp"

#include <nlohmann/json_fwd.hpp>
#include <sockpp/unix_acceptor.h>

#include <functional>

namespace adbfsm::data
{
    namespace ipc
    {
        // clang-format off
        struct Help         {};
        struct SetPageSize  { usize kib; };
        struct GetPageSize  {};
        struct SetCacheSize { usize mib; };
        struct GetCacheSize {};
        // clang-format on

        using Op = Var<Help, SetPageSize, GetPageSize, SetCacheSize, GetCacheSize>;
    }

    class Ipc
    {
    public:
        using OnOp = std::move_only_function<nlohmann::json(ipc::Op op)>;
        static Expect<Ipc> create();

        Ipc() = delete;
        ~Ipc();

        Ipc(Ipc&&)            = default;
        Ipc& operator=(Ipc&&) = default;

        void launch(OnOp on_op);
        void stop();

        path::Path path() const { return m_socket_path.as_path(); }

    private:
        Ipc(path::PathBuf path, Uniq<sockpp::unix_acceptor> acceptor)
            : m_socket_path{ std::move(path) }
            , m_threadpool{ std::make_unique<util::Threadpool>(std::thread::hardware_concurrency(), false) }
            , m_socket{ std::move(acceptor) }
        {
        }

        void run(std::stop_token st, OnOp on_op);

        path::PathBuf               m_socket_path;
        Uniq<util::Threadpool>      m_threadpool;
        Uniq<sockpp::unix_acceptor> m_socket;
        std::jthread                m_thread;
        bool                        m_running = false;    // should have been an atomic
    };
}
