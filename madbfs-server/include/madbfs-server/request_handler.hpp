#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/async/async.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::server
{
    class RequestHandler
    {
    public:
        using Response = Var<rpc::Status, rpc::Response>;

        RequestHandler() = default;

        Response handle_req(rpc::req::Listdir req);
        Response handle_req(rpc::req::Stat req);
        Response handle_req(rpc::req::Readlink req);
        Response handle_req(rpc::req::Mknod req);
        Response handle_req(rpc::req::Mkdir req);
        Response handle_req(rpc::req::Unlink req);
        Response handle_req(rpc::req::Rmdir req);
        Response handle_req(rpc::req::Rename req);
        Response handle_req(rpc::req::Truncate req);
        Response handle_req(rpc::req::Utimens req);
        Response handle_req(rpc::req::CopyFileRange req);
        Response handle_req(rpc::req::Open req);
        Response handle_req(rpc::req::Close req);
        Response handle_req(rpc::req::Read req);
        Response handle_req(rpc::req::Write req);
        Response handle_req(rpc::req::Ping req);

    private:
        bool m_renameat2_impl       = true;
        bool m_copy_file_range_impl = true;

        Array<char, PATH_MAX> m_readlink_buf = {};
    };
}
