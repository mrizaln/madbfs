#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class RequestHandler
    {
    public:
        RequestHandler() = default;

        rpc::FallibleResponse handle_req(rpc::req::Listdir req);
        rpc::FallibleResponse handle_req(rpc::req::Stat req);
        rpc::FallibleResponse handle_req(rpc::req::Readlink req);
        rpc::FallibleResponse handle_req(rpc::req::Mknod req);
        rpc::FallibleResponse handle_req(rpc::req::Mkdir req);
        rpc::FallibleResponse handle_req(rpc::req::Unlink req);
        rpc::FallibleResponse handle_req(rpc::req::Rmdir req);
        rpc::FallibleResponse handle_req(rpc::req::Rename req);
        rpc::FallibleResponse handle_req(rpc::req::Truncate req);
        rpc::FallibleResponse handle_req(rpc::req::Utimens req);
        rpc::FallibleResponse handle_req(rpc::req::CopyFileRange req);
        rpc::FallibleResponse handle_req(rpc::req::Open req);
        rpc::FallibleResponse handle_req(rpc::req::Close req);
        rpc::FallibleResponse handle_req(rpc::req::Read req);
        rpc::FallibleResponse handle_req(rpc::req::Write req);
        rpc::FallibleResponse handle_req(rpc::req::Ping req);

    private:
        bool m_renameat2_impl       = true;
        bool m_copy_file_range_impl = true;

        Array<char, PATH_MAX> m_readlink_buf = {};
    };
}
