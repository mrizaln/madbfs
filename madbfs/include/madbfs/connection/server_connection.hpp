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
         * @return A newly created ServerConnection class.
         */
        static AExpect<Uniq<ServerConnection>> prepare_and_create(Opt<path::Path> server, u16 port);

        ~ServerConnection();

        AExpect<Gen<ParsedStat>> statdir(path::Path path) override;
        AExpect<data::Stat>      stat(path::Path path) override;
        AExpect<path::PathBuf>   readlink(path::Path path) override;

        AExpect<void> mknod(path::Path path) override;
        AExpect<void> mkdir(path::Path path) override;
        AExpect<void> unlink(path::Path path) override;
        AExpect<void> rmdir(path::Path path) override;
        AExpect<void> rename(path::Path from, path::Path to) override;

        AExpect<void>  truncate(path::Path path, off_t size) override;
        AExpect<usize> read(path::Path path, Span<char> out, off_t offset) override;
        AExpect<usize> write(path::Path path, Span<const char> in, off_t offset) override;
        AExpect<void>  utimens(path::Path path, timespec atime, timespec mtime) override;

        AExpect<usize> copy_file_range(path::Path in, off_t in_off, path::Path out, off_t out_off, usize size)
            override;

    private:
        ServerConnection(u16 port)
            : m_port{ port }
        {
        }
        ServerConnection(u16 port, Process proc, Pipe out, Pipe err)
            : m_port{ port }
            , m_server_proc{ std::move(proc) }
            , m_server_out{ std::move(out) }
            , m_server_err{ std::move(err) }
        {
        }

        u16          m_port        = 0;
        Opt<Process> m_server_proc = {};    // server process handle
        Opt<Pipe>    m_server_out  = {};    // server's stdout
        Opt<Pipe>    m_server_err  = {};    // server's stderr
    };
}
