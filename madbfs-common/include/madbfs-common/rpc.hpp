#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"

namespace madbfs::rpc
{
    // based on chirp: https://github.com/creachadair/chirp/blob/main/spec.md

    /*
     * This RPC protocol doesn't define the parameters for each req_params, it only provides set of req_params
     * that the client and server can aggree upon. The parameters are opaque and up to the client and server
     * implementation. PayloadBuilder/PayloadParser can be used to easily create/read the payload from/to C++
     * type system to/from raw bytes. The message protocol is up to the implementation (LV-encoding is the
     * easiest one).
     */

    enum class Procedure : u8
    {
        Listdir = 1,
        Stat,
        Readlink,
        Mkdir,
        Rm,
        Rmdir,
        Mv,
        Truncate,
        Open,
        Read,
        Write,
        Flush,
        Release,
        CopyFileRange,
        Touch,
    };

    // Corresponds to `madbfs::data::Connection` calls
    namespace req_params
    {
        struct Listdir
        {
            Str path;
        };

        struct Stat
        {
            Str path;
        };

        struct Readlink
        {
            Str path;
        };

        struct Mkdir
        {
            Str path;
        };

        struct Rm
        {
            Str  path;
            bool recursive;
        };

        struct Rmdir
        {
            Str path;
        };

        struct Mv
        {
            Str path;
        };

        struct Truncate
        {
            Str path;
            i64 offset;
        };

        struct Open
        {
            Str path;
            i32 flags;
        };

        struct Read
        {
            Str path;
            i64 offset;
            u64 size;
        };

        struct Write
        {
            Str            path;
            i64            offset;
            Span<const u8> in;
        };

        struct Flush
        {
            Str path;
        };

        struct Release
        {
            Str path;
        };

        struct CopyFileRange
        {
            Str in_path;
            i64 in_offset;
            Str out_path;
            i64 out_offset;
            u64 size;
        };
    }

    using ReqParam = Var<
        req_params::Listdir,
        req_params::Stat,
        req_params::Readlink,
        req_params::Mkdir,
        req_params::Rm,
        req_params::Rmdir,
        req_params::Mv,
        req_params::Truncate,
        req_params::Open,
        req_params::Read,
        req_params::Write,
        req_params::Flush,
        req_params::Release>;

    namespace resp_params
    {
    }

    struct Protocol
    {
        AExpect<void> send_req_procedure(Procedure procedure);
        AExpect<void> send_req_param(ReqParam);

        AExpect<Procedure> recv_req_procedure();
        AExpect<ReqParam>  recv_req_param(Procedure procedure);

        AExpect<void> send_resp_procedure(Procedure);
        AExpect<void> recv_resp_procedure(Procedure);
    };
}
