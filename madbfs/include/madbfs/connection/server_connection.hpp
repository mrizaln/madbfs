#include "madbfs-common/rpc.hpp"
#include "madbfs/connection/connection.hpp"

#define BOOST_PROCESS_VERSION 2
#include <boost/process.hpp>

namespace madbfs::connection
{
    class ServerConnection final : public Connection
    {
    public:
        using Process = boost::process::v2::process;
        using Pipe    = async::pipe::Read;

        /**
         * @brief Prepare the server connection and create the class.
         *
         * @param port Port the server will be listening on.
         *
         * The returned Uniq will never be nullptr.
         */
        static AExpect<Uniq<ServerConnection>> prepare_and_create(Opt<path::Path> server, u16 port);

        ~ServerConnection();

        ServerConnection(ServerConnection&&)            = delete;
        ServerConnection& operator=(ServerConnection&&) = delete;

        ServerConnection(const ServerConnection&)            = delete;
        ServerConnection& operator=(const ServerConnection&) = delete;

        AExpect<Gen<ParsedStat>> statdir(path::Path path) override;
        AExpect<data::Stat>      stat(path::Path path) override;
        AExpect<path::PathBuf>   readlink(path::Path path) override;

        AExpect<void> mknod(path::Path path, mode_t mode, dev_t dev) override;
        AExpect<void> mkdir(path::Path path, mode_t mode) override;
        AExpect<void> unlink(path::Path path) override;
        AExpect<void> rmdir(path::Path path) override;
        AExpect<void> rename(path::Path from, path::Path to, u32 flags) override;

        AExpect<void>  truncate(path::Path path, off_t size) override;
        AExpect<usize> read(path::Path path, Span<char> out, off_t offset) override;
        AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) override;
        AExpect<void>  utimens(path::Path path, timespec atime, timespec mtime) override;

        AExpect<usize> copy_file_range(path::Path in, off_t in_off, path::Path out, off_t out_off, usize size)
            override;

    private:
        ServerConnection(u16 port, Uniq<rpc::Client> client)
            : m_port{ port }
            , m_client{ std::move(client) }
        {
        }

        ServerConnection(u16 port, Uniq<rpc::Client> client, Process proc, Pipe out, Pipe err)
            : m_port{ port }
            , m_client{ std::move(client) }
            , m_server_proc{ std::move(proc) }
            , m_server_out{ std::move(out) }
            , m_server_err{ std::move(err) }
        {
        }

        /**
         * @brief Connect to the server and create an RPC client.
         *
         * @param port The port to connect to.
         */
        static AExpect<Uniq<rpc::Client>> make_client(u16 port);

        /**
         * @brief Send request through the RPC client.
         *
         * @param buf Data buffer.
         * @param req Operation request.
         *
         * This function will try to reconnect if the RPC client is disconnected.
         */
        AExpect<rpc::Response> send(Vec<u8>& buf, rpc::Request req);

        /**
         * @brief Request send wrapper.
         *
         * @param buf Data buffer.
         * @param req Operation request.
         *
         * This function typecheck the returned response variant from `send` to match the corresponding
         * request.
         */
        template <rpc::IsRequest Req>
        AExpect<rpc::ToResp<Req>> send_req(Vec<u8>& buf, Req req)
        {
            auto res = co_await send(buf, std::move(req));
            if (not res) {
                co_return Unexpect{ res.error() };
            }

            if (auto resp = std::get_if<rpc::ToResp<Req>>(&*res); resp != nullptr) {
                co_return std::move(*resp);
            }

            co_return Unexpect{ Errc::bad_message };
        }

        u16               m_port        = 0;
        Uniq<rpc::Client> m_client      = nullptr;    // may be null (in the case of disconnection)
        Opt<Process>      m_server_proc = {};         // server process handle
        Opt<Pipe>         m_server_out  = {};         // server's stdout
        Opt<Pipe>         m_server_err  = {};         // server's stderr
    };
}
