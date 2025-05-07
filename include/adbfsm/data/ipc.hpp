#pragma once

#include "adbfsm/common.hpp"
#include "adbfsm/path/path.hpp"
#include "adbfsm/util/threadpool.hpp"

#include <sockpp/unix_acceptor.h>

#include <functional>

namespace adbfsm::data
{
    namespace ipc
    {
        // clang-format off
        struct SetPageSize  { usize kib; };
        struct GetPageSize  {};
        struct SetCacheSize { usize mib; };
        struct GetCacheSize {};
        // clang-format on

        using Op = Var<SetPageSize, GetPageSize, SetCacheSize, GetCacheSize>;
    }

    class Ipc
    {
    public:
        static Expect<Ipc> create();

        void run(std::stop_token st, std::move_only_function<void(ipc::Op op)>);

    private:
        Ipc(path::PathBuf path, Uniq<sockpp::unix_acceptor> acceptor)
            : m_socket_path{ std::move(path) }
            , m_threadpool{ std::make_unique<util::Threadpool>(std::thread::hardware_concurrency(), false) }
            , m_socket{ std::move(acceptor) }
        {
        }

        path::PathBuf               m_socket_path;
        Uniq<util::Threadpool>      m_threadpool;
        Uniq<sockpp::unix_acceptor> m_socket;
    };
}
