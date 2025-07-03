#pragma once

#include "madbfs-common/aliases.hpp"
#include "madbfs-common/async/async.hpp"

#include <saf.hpp>

#include <sys/stat.h>
#include <sys/types.h>

namespace madbfs::rpc
{
    using Socket = async::tcp::Socket;

    template <typename T>
    using Channel = async::Channel<T>;

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

    namespace meta
    {
        template <typename>
        struct VarTraits
        {
        };

        template <template <typename...> typename VT, typename... Ts>
        struct VarTraits<VT<Ts...>>
        {
            static constexpr usize size = sizeof...(Ts);

            template <typename T>
            static constexpr bool has_type = (std::same_as<T, Ts> || ...);

            template <usize I>
            using TypeAt = std::tuple_element_t<I, std::tuple<Ts...>>;    // A hack :D

            template <typename T>
                requires has_type<T>
            static consteval usize type_index()
            {
                auto handler = []<usize... Is>(std::index_sequence<Is...>) {
                    return ((std::same_as<T, Ts> ? Is : 0) + ...);
                };
                return handler(std::make_index_sequence<size>{});
            }

            template <typename T, typename Var>
            using Swap = VarTraits<Var>::template TypeAt<type_index<T>()>;
        };

        template <typename T>
        concept IsRequest = meta::VarTraits<Request>::has_type<T>;

        template <typename T>
        concept IsResponse = meta::VarTraits<Response>::has_type<T>;

        template <IsRequest Req>
        using ToResp = VarTraits<Request>::Swap<Req, Response>;

        template <IsResponse Resp>
        using ToReq = VarTraits<Response>::Swap<Resp, rpc::Request>;
    }

    using meta::IsRequest;
    using meta::IsResponse;
    using meta::ToReq;
    using meta::ToResp;

    class Client
    {
    public:
        using Future = saf::future<Expect<Response>>;

        Client(Socket socket)
            : m_socket{ std::move(socket) }
            , m_channel{ socket.get_executor(), 4096 }    // TODO: bigger numbers?
        {
        }

        Socket& sock() noexcept { return m_socket; }
        bool    running() const { return m_running; }

        Await<void>     start();
        AExpect<Future> send_req(Vec<u8>& buffer, Request req);
        void            stop();

    private:
        struct Promise
        {
            Vec<u8>&                       buffer;
            saf::promise<Expect<Response>> promise;
        };

        using Inflight = std::unordered_map<Id, Promise, Id::Hash>;

        AExpect<void> receive();
        AExpect<void> send();

        Socket                  m_socket;
        Channel<Span<const u8>> m_channel;

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
     *
     * Set client to true if you are client, set client to false if you are server.
     */
    AExpect<void> handshake(Socket& sock, bool client);
}
