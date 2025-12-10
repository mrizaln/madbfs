#pragma once

#include "madbfs/connection/connection.hpp"

#include <unordered_map>

namespace madbfs::connection
{
    /**
     * @class AdbConnection
     *
     * @brief Connection implementation using `adb` proxy.
     */
    class AdbConnection final : public Connection
    {
    public:
        AdbConnection() = default;

        Str          name() const override { return "adb"; }
        Opt<Seconds> timeout() const override { return m_timeout; }
        Opt<Seconds> set_timeout(Opt<Seconds> timeout) override { return std::exchange(m_timeout, timeout); }

        AExpect<Gen<ParsedStat>> statdir(path::Path path) override;
        AExpect<data::Stat>      stat(path::Path path) override;
        AExpect<String>          readlink(path::Path path) override;

        AExpect<void> mknod(path::Path path, mode_t mode, dev_t dev) override;
        AExpect<void> mkdir(path::Path path, mode_t mode) override;
        AExpect<void> unlink(path::Path path) override;
        AExpect<void> rmdir(path::Path path) override;
        AExpect<void> rename(path::Path from, path::Path to, u32 flags) override;
        AExpect<void> truncate(path::Path path, off_t size) override;
        AExpect<void> utimens(path::Path path, timespec atime, timespec mtime) override;

        AExpect<usize> copy_file_range(path::Path in, off_t in_off, path::Path out, off_t out_off, usize size)
            override;

        AExpect<u64>  open(path::Path path, data::OpenMode mode) override;
        AExpect<void> close(u64 fd) override;

        AExpect<usize> read(u64 fd, Span<char> out, off_t offset) override;
        AExpect<usize> write(u64 fd, Span<const char> in, off_t offset) override;

    private:
        using FdMap = std::unordered_map<u64, path::PathBuf>;

        Opt<Seconds> m_timeout;
        u64          m_fd_counter = 0;
        FdMap        m_fd_map;
    };
}
