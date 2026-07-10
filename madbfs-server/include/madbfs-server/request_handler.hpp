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

        rpc::ResponseResult handle_req(rpc::req::Listdir req);
        rpc::ResponseResult handle_req(rpc::req::Stat req);
        rpc::ResponseResult handle_req(rpc::req::Readlink req);
        rpc::ResponseResult handle_req(rpc::req::Mknod req);
        rpc::ResponseResult handle_req(rpc::req::Mkdir req);
        rpc::ResponseResult handle_req(rpc::req::Unlink req);
        rpc::ResponseResult handle_req(rpc::req::Rmdir req);
        rpc::ResponseResult handle_req(rpc::req::Rename req);
        rpc::ResponseResult handle_req(rpc::req::Truncate req);
        rpc::ResponseResult handle_req(rpc::req::Utimens req);
        rpc::ResponseResult handle_req(rpc::req::CopyFileRange req);
        rpc::ResponseResult handle_req(rpc::req::Open req);
        rpc::ResponseResult handle_req(rpc::req::Close req);
        rpc::ResponseResult handle_req(rpc::req::Read req);
        rpc::ResponseResult handle_req(rpc::req::Write req);
        rpc::ResponseResult handle_req(rpc::req::Ping req);

    private:
        bool m_renameat2_impl       = true;
        bool m_copy_file_range_impl = true;

        Array<char, PATH_MAX> m_readlink_buf = {};
    };
}
