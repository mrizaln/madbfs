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

    // NOTE: if you decided to add/remove one or more entries, do update domain check in read_procedure
    enum class Procedure : u8
    {
        Listdir,
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

    class Id
    {
    public:
        using Inner = u32;

        struct Hash
        {
            usize operator()(Id id) const { return std::hash<Inner>{}(id.inner()); }
        };

        Id() = default;

        Id(Inner inner)
            : m_inner{ inner }
        {
        }

        Inner inner() const { return m_inner; }

        auto operator<=>(const Id&) const = default;

    private:
        Inner m_inner = 0;
    };

    namespace req
    {
        // clang-format off
        struct Listdir       { Str path; };
        struct Stat          { Str path; };
        struct Readlink      { Str path; };
        struct Mknod         { Str path; mode_t mode; dev_t dev; };
        struct Mkdir         { Str path; mode_t mode; };
        struct Unlink        { Str path; };
        struct Rmdir         { Str path; };
        struct Rename        { Str from; Str to; u32 flags; };
        struct Truncate      { Str path; off_t size; };
        struct Read          { Str path; off_t offset; usize size; };
        struct Write         { Str path; off_t offset; Span<const u8> in; };
        struct Utimens       { Str path; timespec atime; timespec mtime; };
        struct CopyFileRange { Str in_path; off_t in_offset; Str out_path; off_t out_offset; usize size; };
        // clang-format on
    }

    struct Request    //
        : util::VarWrapper<
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
              req::CopyFileRange>
    {
        // make the base constructor visible
        using VarWrapper::VarWrapper;

        Procedure proc() { return static_cast<Procedure>(index()); }
    };

    namespace resp
    {
        struct Stat;

        struct Listdir
        {
            Vec<Pair<Str, Stat>> entries;
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

    struct Response    //
        : util::VarWrapper<
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
              resp::CopyFileRange>
    {
        // make the base constructor visible
        using VarWrapper::VarWrapper;

        Procedure proc() { return static_cast<Procedure>(index()); }
    };

    template <typename T>
    concept IsRequest = util::VarTraits<Request::Var>::has_type<T>();

    template <typename T>
    concept IsResponse = util::VarTraits<Response::Var>::has_type<T>();

    template <IsRequest Req>
    using ToResp = util::VarTraits<Request::Var>::Swap<Req, Response::Var>;

    template <IsResponse Resp>
    using ToReq = util::VarTraits<Response::Var>::Swap<Resp, rpc::Request::Var>;

    class Client
    {
    public:
        Client(Socket socket)
            : m_socket{ std::move(socket) }
            , m_channel{ socket.get_executor() }
        {
        }

        Socket& sock() noexcept { return m_socket; }
        bool    running() const { return m_running; }

        Await<void>       start();
        AExpect<Response> send_req(Vec<u8>& buffer, Request req, Opt<Milliseconds> timeout);
        void              stop();

    private:
        struct Promise
        {
            Vec<u8>&                       buffer;
            saf::promise<Expect<Response>> promise;
        };

        using Inflight = std::unordered_map<Id, Promise, Id::Hash>;
        using Channel  = async::Channel<Tup<Id, Span<const u8>>>;

        AExpect<void> receive();
        AExpect<void> send();

        Socket  m_socket;
        Channel m_channel;

        Inflight  m_requests;
        Id::Inner m_counter = 0;
        bool      m_running = false;
    };

    class Server
    {
    public:
        using HandlerSig = Await<Var<Status, Response>>(Vec<u8>& buffer, Request request);
        using Handler    = std::function<HandlerSig>;

        Server(Socket socket)
            : m_socket{ std::move(socket) }
        {
        }

        Socket& sock() noexcept { return m_socket; }

        AExpect<void> listen(Handler handler);
        void          stop();

    private:
        AExpect<void> send_resp(Id id, Procedure proc, Var<Status, Response> response);

        Socket m_socket;
        bool   m_running;
    };

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
}
