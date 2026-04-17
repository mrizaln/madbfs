#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/util/var_wrapper.hpp"

#include <saf.hpp>

#include <sys/stat.h>
#include <sys/types.h>

namespace madbfs::rpc
{
    using Socket = async::tcp::Socket;
    using Status = std::errc;

    static_assert(sizeof(Status) == 4, "huh, unusual system. usually enums without base (int) are 4 bytes.");

    // NOTE: if you decide to add/remove one or more entries, do update domain check in read_procedure
    enum class Procedure : u8
    {
        Stat,
        Listdir,
        Readlink,
        Mknod,
        Mkdir,
        Unlink,
        Rmdir,
        Rename,
        Truncate,
        Utimens,
        CopyFileRange,
        Open,
        Close,
        Read,
        Write,
        Ping,    // special procedure for checking aliveness
    };

    enum class OpenMode : u8
    {
        Read      = 0,
        Write     = 1,
        ReadWrite = 2,
    };

    /**
     * @class Id
     *
     * @brief Identifies RPC request/response.
     */
    class Id
    {
    public:
        using Inner = u32;

        struct Hash
        {
            constexpr usize operator()(Id id) const { return std::hash<Inner>{}(id.inner()); }
        };

        constexpr Id() = default;

        constexpr Id(Inner inner)
            : m_inner{ inner }
        {
        }

        constexpr Inner inner() const { return m_inner; }

        constexpr auto operator<=>(const Id&) const = default;

    private:
        Inner m_inner = 0;
    };

    namespace req
    {
        // clang-format off
        struct Stat          { Str path; };
        struct Listdir       { Str path; Vec<u8>& buf; };
        struct Readlink      { Str path; Vec<u8>& buf; };
        struct Mknod         { Str path; mode_t mode; dev_t dev; };
        struct Mkdir         { Str path; mode_t mode; };
        struct Unlink        { Str path; };
        struct Rmdir         { Str path; };
        struct Rename        { Str from; Str to; u32 flags; };
        struct Truncate      { Str path; off_t size; };
        struct Utimens       { Str path; timespec atime; timespec mtime; };
        struct CopyFileRange { Str in_path; off_t in_offset; Str out_path; off_t out_offset; usize size; };
        struct Open          { Str path; OpenMode mode; };
        struct Close         { u64 fd; };
        struct Read          { u64 fd; off_t offset; Span<u8> out; };
        struct Write         { u64 fd; off_t offset; Span<const u8> in; };
        struct Ping          { u64 num; };
        // clang-format on
    }

    struct Request    //
        : util::VarWrapper<
              req::Stat,
              req::Listdir,
              req::Readlink,
              req::Mknod,
              req::Mkdir,
              req::Unlink,
              req::Rmdir,
              req::Rename,
              req::Truncate,
              req::Utimens,
              req::CopyFileRange,
              req::Open,
              req::Close,
              req::Read,
              req::Write,
              req::Ping>
    {
        // make the base constructor visible
        using VarWrapper::VarWrapper;

        Procedure proc() const { return static_cast<Procedure>(index()); }
    };

    struct NamedRequest
    {
        Id      id;
        Request req;
    };

    struct RequestHeader
    {
        Id        id;
        Procedure proc;
        u64       size;
    };

    namespace resp
    {
        // clang-format off
        struct Stat;
        struct Listdir       { Vec<Pair<Str, Stat>> entries; }; // uses corresponding `req::Listdir` buf
        struct Readlink      { Str target; };                   // uses corresponding `req::Readlink` buf 
        struct Mknod         { };
        struct Mkdir         { };
        struct Unlink        { };
        struct Rmdir         { };
        struct Rename        { };
        struct Truncate      { };
        struct Utimens       { };
        struct CopyFileRange { usize size; };
        struct Open          { u64 fd; };
        struct Close         { };
        struct Read          { Span<const u8> read; };          // uses corresponding `req::Read` out
        struct Write         { usize size; };
        struct Ping          { u64 num; };
        // clang-format on

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
    }

    struct Response    //
        : util::VarWrapper<
              resp::Stat,
              resp::Listdir,
              resp::Readlink,
              resp::Mknod,
              resp::Mkdir,
              resp::Unlink,
              resp::Rmdir,
              resp::Rename,
              resp::Truncate,
              resp::Utimens,
              resp::CopyFileRange,
              resp::Open,
              resp::Close,
              resp::Read,
              resp::Write,
              resp::Ping>
    {
        // make the base constructor visible
        using VarWrapper::VarWrapper;

        Procedure proc() const { return static_cast<Procedure>(index()); }
    };

    struct ResponseHeader
    {
        Id        id;
        Procedure proc;
        Status    status;
        u64       size;
    };

    template <typename T>
    concept IsRequest = util::VarTraits<Request::Var>::has_type<T>();

    template <typename T>
    concept IsResponse = util::VarTraits<Response::Var>::has_type<T>();

    template <IsRequest Req>
    using ToResp = util::VarTraits<Request::Var>::Swap<Req, Response::Var>;

    template <IsResponse Resp>
    using ToReq = util::VarTraits<Response::Var>::Swap<Resp, rpc::Request::Var>;

    static constexpr Str server_ready_string = "SERVER_IS_READY";

    /**
     * @brief Return string representation of enum Procedure.
     *
     * The string lifetime is static.
     */
    Str to_string(Procedure procedure);

    /**
     * @brief Return the type name of the contained Request variant.
     *
     * The string lifetime is static.
     */
    Str to_string(Request request);

    /**
     * @brief Return the type name of the contained Response variant.
     *
     * The string lifetime is static.
     */
    Str to_string(Response response);

    /**
     * @brief Do a handshake with remote connection.
     */
    AExpect<void> handshake(Socket& sock);

    /**
     * @brief Serialize then send request through socket.
     *
     * @param socket The socket in which the serialized request will be sent.
     * @param buffer Storage for serialization.
     * @param request The request to be sent.
     * @param id Unique request identifier.
     */
    AExpect<void> send_request(Socket& socket, Vec<u8>& buffer, Request request, Id id);

    /**
     * @brief Serialize then send response through socket.
     *
     * @param socket The socket in which the serialized request will be sent.
     * @param buffer Storage for serialization.
     * @param proc Response procedure.
     * @param response Response data for the procedure.
     * @param id Response Unique response identifier.
     */
    AExpect<void> send_response(
        Socket&               socket,
        Vec<u8>&              buffer,
        Procedure             proc,
        Var<Status, Response> response,
        Id                    id
    );

    /**
     * @brief Read request header from socket.
     *
     * @param socket The socket to be read from.
     */
    AExpect<RequestHeader> receive_request_header(Socket& socket);

    /**
     * @brief Read response header from socket.
     *
     * @param socket The socket to be read from.
     */
    AExpect<ResponseHeader> receive_response_header(Socket& socket);

    /**
     * @brief Read request payload with information from the header.
     *
     * @param socket The socket to be read from.
     * @param buffer Storage for request payload.
     * @param header Valid request header.
     *
     * The `buffer` is both used for receiving the payload and for output buffer to be filled later for some
     * procedures. The `buffer` must live long enough for any user that uses the resulting request.
     */
    AExpect<Request> receive_request(Socket& socket, Vec<u8>& buffer, RequestHeader header);

    /**
     * @brief Read response payload with information from the header.
     *
     * @param socket The socket to be read from.
     * @param buffer Storage for response payload.
     * @param header Valid request header.
     * @param req Associated request struct for the response.
     *
     * The `req` is required since some procedures have an output buffer (for string/bytes data). To prevent
     * unnecessary copy, the function will fill the data directly into the output buffer. The lifetime of the
     * output buffer of the request is tied to the response and the request.
     *
     * The `buffer` is required for parsing non-string/non-bytes payloads, it may be destroyed/reused
     * immediately after this function returns.
     */
    AExpect<Response> receive_response(Socket& socket, Vec<u8>& buffer, ResponseHeader header, Request req);
}
