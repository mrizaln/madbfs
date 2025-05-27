#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"

#include <sys/stat.h>
#include <sys/types.h>

namespace madbfs::rpc
{
    /*
     * This RPC is opaque from both the client and the server. The stub is provided as is and provides correct
     * semantics if provided that the caller and callee conforms to the contract:
     *
     * client:
     * - call to `send_req_procedure` with certain procedure
     * - must be followed by a call to `send_req_param` with appropriate param in req namespace
     * - read the response by calling `recv_resp_procedure`
     *
     * server:
     * - call to `recv_req_procedure`
     * - followed by call to `recv_req_param` with previously obtained procedure enum
     * - response with `send_resp_procedure`
     *
     * For Listdir procedure only:
     * - server must send dirent continuously using `listdir_channel::Sender::send_next`, send EOF on complete
     * - client must recv dirent continuously using `listdir_channel::Receiver::recv_next` until EOF
     */

    using Socket = async::tcp::Socket;

    enum class Procedure : u8
    {
        Listdir = 1,
        Stat,
        Readlink,
        Mknod,
        Mkdir,
        Unlink,
        Rmdir,
        Rename,
        Truncate,
        Read,
        Write,
        Utimens,
        CopyFileRange,
    };

    enum class Status : u8
    {
        Success               = 0,
        NoSuchFileOrDirectory = ENOENT,
        PermissionDenied      = EACCES,
        FileExists            = EEXIST,
        NotADirectory         = ENOTDIR,
        IsADirectory          = EISDIR,
        InvalidArgument       = EINVAL,    // generic error
        DirectoryNotEmpty     = ENOTEMPTY,
    };

    // Corresponds to `madbfs::data::Connection` calls
    namespace req
    {
        // NOTE: server after receiving this request must immediately use `listdir_channel::Sender` and send
        // the directory data by that channel.
        struct Listdir
        {
            Str path;
        };

        // clang-format off
        struct Stat          { Str path; };
        struct Readlink      { Str path; };
        struct Mknod         { Str path; };
        struct Mkdir         { Str path; };
        struct Unlink        { Str path; };
        struct Rmdir         { Str path; };
        struct Rename        { Str from; Str to; };
        struct Truncate      { Str path; i64 offset; };
        struct Read          { Str path; i64 offset; u64 size; };
        struct Write         { Str path; i64 offset; Span<const u8> in; };
        struct Utimens       { Str path; timespec atime; timespec mtime; };
        struct CopyFileRange { Str in_path; i64 in_offset; Str out_path; i64 out_offset; u64 size; };
        // clang-format on
    }

    using Request = Var<
        req::Listdir,
        req::Stat,
        req::Readlink,
        req::Mknod,
        req::Mkdir,
        req::Unlink,
        req::Rmdir,
        req::Rename,
        req::Truncate,
        req::Read,
        req::Write,
        req::Utimens,
        req::CopyFileRange>;

    namespace resp
    {
        // NOTE: empty. client must use `listdir_channel::Receiver` immediately after getting this
        // response. this is done in order to allow asynchronous stat read for each file while doing
        // iterating the directory.
        struct Listdir
        {
        };

        struct Stat
        {
            off_t    size;
            nlink_t  links;
            timespec mtime;
            timespec atime;
            timespec ctime;
            mode_t   mode;
            uid_t    uid;
            gid_t    gid;
        };

        // clang-format off
        struct Readlink         { Str target; };
        struct Mkdir            { };
        struct Mknod            { };
        struct Unlink           { };
        struct Rmdir            { };
        struct Rename           { };
        struct Truncate         { };
        struct Read             { Span<const u8> read; };
        struct Write            { usize size; };
        struct Utimens          { };
        struct CopyFileRange    { usize size; };
        // clang-format on
    }

    using Response = Var<
        resp::Listdir,
        resp::Stat,
        resp::Readlink,
        resp::Mknod,
        resp::Mkdir,
        resp::Unlink,
        resp::Rmdir,
        resp::Rename,
        resp::Truncate,
        resp::Read,
        resp::Write,
        resp::Utimens,
        resp::CopyFileRange>;

    class Client
    {
    public:
        Client(Socket& socket)
            : m_socket{ socket }
        {
        }

        AExpect<void> send_req_procedure(Procedure procedure);
        AExpect<void> send_req_param(Request request);

        // NOTE: network error won't overlap with procedure error. procedure error are only limited to the
        // integral values defined by Status
        AExpect<Response> recv_resp_procedure(Procedure procedure);

    private:
        Socket& m_socket;
        Vec<u8> m_buffer;
    };

    class Server
    {
    public:
        Server(Socket& socket)
            : m_socket{ socket }
        {
        }

        AExpect<Procedure> recv_req_procedure();
        AExpect<Request>   recv_req_param(Procedure procedure);

        AExpect<void> send_resp_procedure(Var<Status, Response> response);

    private:
        Socket& m_socket;
        Vec<u8> m_buffer;
    };

    namespace listdir_channel
    {
        struct Dirent
        {
            Str      name;
            nlink_t  links;
            off_t    size;
            timespec mtime;
            timespec atime;
            timespec ctime;
            mode_t   mode;
            uid_t    uid;
            gid_t    gid;
        };

        class Sender
        {
        public:
            Sender(Socket& socket)
                : m_socket{ socket }
            {
            }

            AExpect<void> send_next(Dirent dirent);

        private:
            Socket& m_socket;
        };

        class Receiver
        {
        public:
            Receiver(Socket& socket, Vec<u8>& buffer)
                : m_socket{ socket }
                , m_buffer{ buffer }
            {
            }

            /**
             * @brief Receive next directory entry.
             *
             * The  return value will be Errc::end_of_file if
             */
            AExpect<Dirent> recv_next();

        private:
            Socket&  m_socket;
            Vec<u8>& m_buffer;
        };
    }
}
