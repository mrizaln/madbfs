#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/util/var_wrapper.hpp"

#include <saf.hpp>

#include <sys/stat.h>
#include <sys/types.h>

/**
 * This is a low-level RPC mechanism for communication between madbfs client and madbfs server on device. The
 * design decision for this RPC is to be as simple as possible and while also type safe and flexible by giving
 * the user the control of the buffer being used in each request/response invocation.
 *
 * The flow of using this RPC is the following.
 *
 * For server:
 * - Create an acceptor socket with a specified port number.
 * - When a connection is received do a `rpc::handshake()`. Maintain the connection if success.
 * - Create a loop that will become the request handler.
 * - Within it:
 *   - Call `rpc::receive_request_header()`.
 *   - If success, you need to receive the full `Request` payload by calliing `rpc::receive_request()` with
 *     the corresponding header.
 *   - Handle the `Request` on the loop itself or on another thread.
 *   - After handling the `Request`, you should have the corresponding `ResponseResult`.
 *   - Send the response using `rpc::send_response()`.
 *
 * For client:
 * - Create a Socket.
 * - Connect to the server with the specified port number.
 * - Do a `rpc::handshake()` if connection is successful.
 * - After handshake is success, you can do the fullowing until the conneciton is lost:
 *   - Create a `Request` and an `Id`, then send it via `rpc::send_request()`.
 *   - Some `Request` require a buffer that will be used for the corresponding `Response`, which means you
 *     need to hold onto that buffer until a `Response` with the same `Id` is received. After that, the
 *     lifetime of the backing buffer must match or exceed the `Response`.
 *   - You also need to make a copy of the `Request` instance itself that is being sent for later.
 *   - To receive the response, you call `rpc::receive_response_header()` first. This header should contain
 *     the request `Id` in which you can check whether the `Request` fails. If it fails, the corresponding
 *     buffer for the `Request` can be released.
 *   - If succeeds, you can call `rpc::receive_response()` with you supplying the same `Request` before in
 *     order for the function to be able to fill the aforementioned backing buffer.
 *
 * Note:
 * - As explained in the 'For client' section, some `Request` require a buffer for the corresponding
 *   `Response`, and `Request` must be held onto for `rpc::receive_response()` call. I want to clarify that
 *   both the `Request` and `Request` do not own the buffer and only references them (whether via a reference
 *   or span). Thus, this also means that copying `Request` and `Response` should be cheap as they are only a
 *   view to a buffer. The exception is for `rpc::resp::Listdir` which contains a vector of string view into
 *   buffer on its corresponding `rpc::req::Listdir` instance you supplied.
 */
namespace madbfs::rpc
{
    using Socket = async::tcp::Socket;
    using Status = std::errc;

    static_assert(sizeof(Status) == 4, "huh, unusual system. usually enums without base (int) are 4 bytes.");

    /**
     * @brief String used for handshake.
     */
    static constexpr Str server_ready_string = "SERVER_IS_READY";

    /**
     * @enum Procedure
     *
     * @brief Enumerate all possible filesystem operations via this RPC.
     */
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

    /**
     * @class Request
     *
     * @brief Represent a filesystem operations as request structs.
     */
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

        /**
         * @brief Get the `Procedure` enum.
         */
        Procedure proc() const { return static_cast<Procedure>(index()); }
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

    /**
     * @class Response
     *
     * @brief Represent a filesystem operation resultss as response structs.
     */
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

        /**
         * @brief Get the `Procedure` enum.
         */
        Procedure proc() const { return static_cast<Procedure>(index()); }
    };

    struct ResponseHeader
    {
        Id        id;
        Procedure proc;
        Status    status;
        u64       size;
    };

    struct FailedResponse
    {
        Procedure proc;
        Status    status;
    };

    using ResponseResult = Var<Response, FailedResponse>;

    /**
     * @brief Check whether a type is a variant of `Request`.
     */
    template <typename T>
    concept IsRequest = util::VarTraits<Request::Var>::has_type<T>();

    /**
     * @brief Check whether a type is a variant of `Response`.
     */
    template <typename T>
    concept IsResponse = util::VarTraits<Response::Var>::has_type<T>();

    /**
     * @brief Convert `Request` variant into its corresponding `Response` variant.
     */
    template <IsRequest Req>
    using ToResp = util::VarTraits<Request::Var>::Swap<Req, Response::Var>;

    /**
     * @brief Convert `Response` variant into its corresponding `Request` variant.
     */
    template <IsResponse Resp>
    using ToReq = util::VarTraits<Response::Var>::Swap<Resp, rpc::Request::Var>;

    /**
     * @brief Get `Procedure` enum from request or response variant.
     */
    template <typename T>
        requires (IsRequest<T> or IsResponse<T>)
    Procedure to_proc()
    {
        using R = std::conditional_t<IsRequest<T>, Request, Response>;
        return static_cast<Procedure>(R::template index_of<T>());
    }

    /**
     * @brief Get `Procedure` enum from request or response variant.
     */
    template <typename T>
        requires (IsRequest<T> or IsResponse<T>)
    Procedure to_proc(const T&)
    {
        return to_proc<T>();
    };

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
    AExpect<void> send_response(Socket& socket, Vec<u8>& buffer, ResponseResult response, Id id);

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
     * The `buffer` is required for receiving and parsing the Response payload, it may be destroyed/reused
     * immediately after this function returns.
     *
     * The `req` is required since some procedures have an output buffer (for string/bytes data). The function
     * will copy the data into the output buffer after received. The lifetime of the output buffer of the
     * request is tied to the response and the request.
     */
    AExpect<Response> receive_response(Socket& socket, Vec<u8>& buffer, ResponseHeader header, Request req);
}
