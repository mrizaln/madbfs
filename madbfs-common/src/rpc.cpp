#include "madbfs-common/rpc.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/util/overload.hpp"

// TODO: add log
#define HANDLE_ERROR(Ec, Size, Want)                                                                         \
    if (Ec) {                                                                                                \
        co_return madbfs::Unexpect{ madbfs::async::to_generic_err(Ec) };                                     \
    } else if (Size != Want) {                                                                               \
        co_return madbfs::Unexpect{ madbfs::Errc::broken_pipe };                                             \
    }

// NOTE: if only C++ has '?' a la rust... this is the most I can think of for C++
#define TRY_SCOPED(Stmt)                                                                                     \
    if (auto res = Stmt; not res) {                                                                          \
        co_return Unexpect{ res.error() };                                                                   \
    }

#define TRY(Name, Stmt)                                                                                      \
    auto Name = Stmt;                                                                                        \
    if (not Name) {                                                                                          \
        co_return Unexpect{ Name.error() };                                                                  \
    }

namespace
{
    using namespace madbfs;
    using namespace madbfs::rpc;

    struct Slice
    {
        isize offset;
        usize size;
    };

    Str slice_as_str(Span<const u8> buf, Slice slice)
    {
        return Str{ reinterpret_cast<const char*>(buf.data() + slice.offset), slice.size };
    }

    Span<const u8> slice_bytes(Span<const u8> buf, Slice slice)
    {
        return buf.subspan(static_cast<usize>(slice.offset), slice.size);
    }

    template <std::integral I>
    std::array<u8, sizeof(I)> to_net_bytes(I value)
    {
        constexpr auto size = sizeof(I);

        // if constexpr (std::endian::native == std::endian::big) {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
            return std::bit_cast<std::array<u8, size>>(value);
        } else {
            auto array = std::bit_cast<std::array<u8, size>>(value);
            for (auto i = 0u; i < size / 2; ++i) {
                std::swap(array[i], array[size - i - 1]);
            }
            return array;
        }
    }

    template <std::integral I>
    I from_net_bytes(std::array<u8, sizeof(I)> bytes)
    {
        constexpr auto size = sizeof(I);

        // if constexpr (std::endian::native == std::endian::big) {
        if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
            return std::bit_cast<I>(bytes);
        } else {
            for (auto i = 0u; i < size / 2; ++i) {
                std::swap(bytes[i], bytes[size - i - 1]);
            }
            return std::bit_cast<I>(bytes);
        }
    }

    template <std::integral I>
    constexpr I swap_endian(const I& value) noexcept
    {
        auto arr = to_net_bytes(value);
        return std::bit_cast<I>(arr);
    }

    template <std::integral I>
    AExpect<void> write_int(Socket& sock, I value)
    {
        auto array      = to_net_bytes(value);
        auto [ec, size] = co_await madbfs::async::write_exact<u8>(sock, array);
        HANDLE_ERROR(ec, size, array.size());
        co_return Expect<void>{};
    }

    AExpect<void> write_procedure(Socket& sock, Procedure procedure)
    {
        return write_int<u8>(sock, static_cast<u8>(procedure));
    }

    AExpect<void> write_status(Socket& sock, Status status)
    {
        return write_int<u8>(sock, static_cast<u8>(status));
    }

    AExpect<void> write_bytes(Socket& sock, Span<const u8> bytes)
    {
        TRY_SCOPED(co_await write_int<u64>(sock, bytes.size()));
        auto [ec, size] = co_await madbfs::async::write_exact<u8>(sock, bytes);
        HANDLE_ERROR(ec, size, bytes.size());
        co_return Expect<void>{};
    }

    AExpect<void> write_path(Socket& sock, Str path)
    {
        TRY_SCOPED(co_await write_int<u64>(sock, path.size()));
        auto [ec_path, n_path] = co_await madbfs::async::write_exact<char>(sock, path);
        HANDLE_ERROR(ec_path, n_path, path.size());

        co_return Expect<void>{};
    };

    template <std::integral I>
    AExpect<I> read_int(Socket& sock)
    {
        auto buf     = Array<u8, sizeof(I)>{};
        auto [ec, n] = co_await async::read_exact<u8>(sock, buf);
        HANDLE_ERROR(ec, n, buf.size());
        co_return from_net_bytes<I>(buf);
    }

    AExpect<Procedure> read_procedure(Socket& sock)
    {
        TRY(byte, co_await read_int<u8>(sock));
        auto proc = static_cast<Procedure>(*byte);

        switch (proc) {
        case Procedure::Listdir:
        case Procedure::Stat:
        case Procedure::Readlink:
        case Procedure::Mknod:
        case Procedure::Mkdir:
        case Procedure::Unlink:
        case Procedure::Rmdir:
        case Procedure::Rename:
        case Procedure::Truncate:
        case Procedure::Read:
        case Procedure::Write:
        case Procedure::Utimens:
        case Procedure::CopyFileRange: co_return proc;
        }

        co_return Unexpect{ Errc::invalid_argument };
    }

    AExpect<void> read_status(Socket& sock)
    {
        TRY(status, co_await read_int<u8>(sock));
        co_return status == 0 ? Expect<void>{} : Unexpect{ static_cast<Errc>(*status) };
    }

    AExpect<Slice> read_bytes(Socket& sock, Vec<u8>& buf)
    {
        TRY(size, co_await read_int<u64>(sock));

        auto off = static_cast<isize>(buf.size());
        buf.resize(buf.size() + *size);
        auto out = Span{ buf.begin() + off, *size };

        auto [ec, n] = co_await async::read_exact<u8>(sock, out);
        HANDLE_ERROR(ec, n, size);
        co_return Slice{ off, n };
    }

    AExpect<Slice> read_path(Socket& sock, Vec<u8>& buf)
    {
        TRY(size, co_await read_int<u64>(sock));

        auto off = static_cast<isize>(buf.size());
        buf.resize(buf.size() + *size + 1);    // null terminator
        auto out = Span{ buf.begin() + off, *size };

        auto [ec, n] = co_await async::read_exact<u8>(sock, out);
        HANDLE_ERROR(ec, n, size);

        buf.push_back('\0');    // null terminator
        co_return Slice{ off, n };
    }
}

namespace madbfs::rpc
{
    // client send request
    // -------------------

    AExpect<resp::Listdir> Client::send_req_listdir(req::Listdir req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Listdir));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));

        TRY(size, co_await read_int<u64>(m_socket));
        auto slices = Vec<Pair<Slice, resp::Stat>>{};
        slices.reserve(*size);

        for (auto _ : sv::iota(0uz, *size)) {
            TRY(slice, co_await read_bytes(m_socket, m_buffer));
            TRY(size, co_await read_int<i64>(m_socket));
            TRY(links, co_await read_int<u64>(m_socket));
            TRY(mtime_sec, co_await read_int<i64>(m_socket));
            TRY(mtime_nsec, co_await read_int<i64>(m_socket));
            TRY(atime_sec, co_await read_int<i64>(m_socket));
            TRY(atime_nsec, co_await read_int<i64>(m_socket));
            TRY(ctime_sec, co_await read_int<i64>(m_socket));
            TRY(ctime_nsec, co_await read_int<i64>(m_socket));
            TRY(mode, co_await read_int<u32>(m_socket));
            TRY(uid, co_await read_int<u32>(m_socket));
            TRY(gid, co_await read_int<u32>(m_socket));

            slices.emplace_back(
                *slice,
                resp::Stat{
                    .size  = *size,
                    .links = static_cast<nlink_t>(*links),
                    .mtime = { .tv_sec = *mtime_sec, .tv_nsec = *mtime_nsec },
                    .atime = { .tv_sec = *atime_sec, .tv_nsec = *atime_nsec },
                    .ctime = { .tv_sec = *ctime_sec, .tv_nsec = *ctime_nsec },
                    .mode  = *mode,
                    .uid   = *uid,
                    .gid   = *gid,
                }
            );
        }

        auto entries = Vec<Pair<Str, resp::Stat>>{};
        entries.reserve(*size);

        for (auto&& [slice, stat] : std::move(slices)) {
            entries.emplace_back(slice_as_str(m_buffer, slice), std::move(stat));
        }

        co_return resp::Listdir{ .entries = std::move(entries) };
    }

    AExpect<resp::Stat> Client::send_req_stat(req::Stat req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Stat));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        TRY(size, co_await read_int<i64>(m_socket));
        TRY(links, co_await read_int<u64>(m_socket));
        TRY(mtime_sec, co_await read_int<i64>(m_socket));
        TRY(mtime_nsec, co_await read_int<i64>(m_socket));
        TRY(atime_sec, co_await read_int<i64>(m_socket));
        TRY(atime_nsec, co_await read_int<i64>(m_socket));
        TRY(ctime_sec, co_await read_int<i64>(m_socket));
        TRY(ctime_nsec, co_await read_int<i64>(m_socket));
        TRY(mode, co_await read_int<u32>(m_socket));
        TRY(uid, co_await read_int<u32>(m_socket));
        TRY(gid, co_await read_int<u32>(m_socket));

        co_return resp::Stat{
            .size  = *size,
            .links = static_cast<nlink_t>(*links),
            .mtime = { .tv_sec = *mtime_sec, .tv_nsec = *mtime_nsec },
            .atime = { .tv_sec = *atime_sec, .tv_nsec = *atime_nsec },
            .ctime = { .tv_sec = *ctime_sec, .tv_nsec = *ctime_nsec },
            .mode  = *mode,
            .uid   = *uid,
            .gid   = *gid,
        };
    }

    AExpect<resp::Readlink> Client::send_req_readlink(req::Readlink req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Readlink));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        TRY(slice, co_await read_bytes(m_socket, m_buffer));
        co_return resp::Readlink{ .target = slice_as_str(m_buffer, *slice) };
    }

    AExpect<resp::Mknod> Client::send_req_mknod(req::Mknod req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Mknod));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Mknod{};
    }

    AExpect<resp::Mkdir> Client::send_req_mkdir(req::Mkdir req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Mkdir));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Mkdir{};
    }

    AExpect<resp::Unlink> Client::send_req_unlink(req::Unlink req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Unlink));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Unlink{};
    }

    AExpect<resp::Rmdir> Client::send_req_rmdir(req::Rmdir req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Rmdir));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Rmdir{};
    }

    AExpect<resp::Rename> Client::send_req_rename(req::Rename req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Rename));
        TRY_SCOPED(co_await write_path(m_socket, req.from));
        TRY_SCOPED(co_await write_path(m_socket, req.to));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Rename{};
    }

    AExpect<resp::Truncate> Client::send_req_truncate(req::Truncate req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Truncate));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.size));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Truncate{};
    }

    AExpect<resp::Read> Client::send_req_read(req::Read req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Read));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.offset));
        TRY_SCOPED(co_await write_int<u64>(m_socket, req.size));
        TRY_SCOPED(co_await read_status(m_socket));
        TRY(slice, co_await read_bytes(m_socket, m_buffer));
        co_return resp::Read{ .read = slice_bytes(m_buffer, *slice) };
    }

    AExpect<resp::Write> Client::send_req_write(req::Write req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Write));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.offset));
        TRY_SCOPED(co_await write_bytes(m_socket, req.in));
        TRY_SCOPED(co_await read_status(m_socket));
        TRY(size, co_await read_int<u64>(m_socket));
        co_return resp::Write{ .size = *size };
    }

    AExpect<resp::Utimens> Client::send_req_utimens(req::Utimens req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::Utimens));
        TRY_SCOPED(co_await write_path(m_socket, req.path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.atime.tv_sec));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.atime.tv_nsec));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.mtime.tv_sec));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.mtime.tv_nsec));
        TRY_SCOPED(co_await read_status(m_socket));
        co_return resp::Utimens{};
    }

    AExpect<resp::CopyFileRange> Client::send_req_copy_file_range(req::CopyFileRange req)
    {
        TRY_SCOPED(co_await write_procedure(m_socket, Procedure::CopyFileRange));
        TRY_SCOPED(co_await write_path(m_socket, req.in_path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.in_offset));
        TRY_SCOPED(co_await write_path(m_socket, req.out_path));
        TRY_SCOPED(co_await write_int<i64>(m_socket, req.out_offset));
        TRY_SCOPED(co_await write_int<u64>(m_socket, req.size));
        TRY_SCOPED(co_await read_status(m_socket));
        TRY(size, co_await read_int<u64>(m_socket));
        co_return resp::CopyFileRange{ .size = *size };
    }
}

namespace madbfs::rpc
{
    // server read request
    // -------------------

    AExpect<req::Listdir> Server::recv_req_listdir()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Listdir{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Stat> Server::recv_req_stat()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Stat{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Readlink> Server::recv_req_readlink()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Readlink{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Mknod> Server::recv_req_mknod()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Mknod{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Mkdir> Server::recv_req_mkdir()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Mkdir{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Unlink> Server::recv_req_unlink()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Unlink{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Rmdir> Server::recv_req_rmdir()
    {
        TRY(slice, co_await read_path(m_socket, m_buffer));
        co_return req::Rmdir{ .path = slice_as_str(m_buffer, *slice) };
    }

    AExpect<req::Rename> Server::recv_req_rename()
    {
        TRY(from_slice, co_await read_path(m_socket, m_buffer));
        TRY(to_slice, co_await read_path(m_socket, m_buffer));

        co_return req::Rename{
            .from = slice_as_str(m_buffer, *from_slice),
            .to   = slice_as_str(m_buffer, *to_slice),
        };
    }

    AExpect<req::Truncate> Server::recv_req_truncate()
    {
        TRY(path_slice, co_await read_path(m_socket, m_buffer));
        TRY(size, co_await read_int<i64>(m_socket));

        co_return req::Truncate{
            .path = slice_as_str(m_buffer, *path_slice),
            .size = *size,
        };
    }

    AExpect<req::Read> Server::recv_req_read()
    {
        TRY(path_slice, co_await read_path(m_socket, m_buffer));
        TRY(offset, co_await read_int<i64>(m_socket));
        TRY(size, co_await read_int<u64>(m_socket));

        co_return req::Read{
            .path   = slice_as_str(m_buffer, *path_slice),
            .offset = *offset,
            .size   = *size,
        };
    }

    AExpect<req::Write> Server::recv_req_write()
    {
        TRY(path_slice, co_await read_path(m_socket, m_buffer));
        TRY(offset, co_await read_int<i64>(m_socket));
        TRY(bytes_slice, co_await read_bytes(m_socket, m_buffer));

        co_return req::Write{
            .path   = slice_as_str(m_buffer, *path_slice),
            .offset = *offset,
            .in     = slice_bytes(m_buffer, *bytes_slice),
        };
    }

    AExpect<req::Utimens> Server::recv_req_utimens()
    {
        TRY(path_slice, co_await read_path(m_socket, m_buffer));
        TRY(atime_sec_slice, co_await read_int<i64>(m_socket));
        TRY(atime_nsec_slice, co_await read_int<i64>(m_socket));
        TRY(mtime_sec_slice, co_await read_int<i64>(m_socket));
        TRY(mtime_nsec_slice, co_await read_int<i64>(m_socket));

        co_return req::Utimens{
            .path  = slice_as_str(m_buffer, *path_slice),
            .atime = { .tv_sec = *atime_sec_slice, .tv_nsec = *atime_nsec_slice },
            .mtime = { .tv_sec = *mtime_sec_slice, .tv_nsec = *mtime_nsec_slice },
        };
    }

    AExpect<req::CopyFileRange> Server::recv_req_copy_file_range()
    {
        TRY(in_slice, co_await read_path(m_socket, m_buffer));
        TRY(in_offset, co_await read_int<i64>(m_socket));
        TRY(out_slice, co_await read_path(m_socket, m_buffer));
        TRY(out_offset, co_await read_int<i64>(m_socket));
        TRY(size, co_await read_int<u64>(m_socket));

        co_return req::CopyFileRange{
            .in_path    = slice_as_str(m_buffer, *in_slice),
            .in_offset  = *in_offset,
            .out_path   = slice_as_str(m_buffer, *out_slice),
            .out_offset = *out_offset,
            .size       = *size,
        };
    }

    // -------------------

    // server send response
    // --------------------
    AExpect<void> write_resp_listdir(Socket& sock, resp::Listdir&& resp)
    {
        TRY_SCOPED(co_await write_int<u64>(sock, resp.entries.size()));

        for (const auto& [name, stat] : resp.entries) {
            TRY_SCOPED(co_await write_path(sock, name));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.size));
            TRY_SCOPED(co_await write_int<u64>(sock, stat.links));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.mtime.tv_sec));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.mtime.tv_nsec));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.atime.tv_sec));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.atime.tv_nsec));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.ctime.tv_sec));
            TRY_SCOPED(co_await write_int<i64>(sock, stat.ctime.tv_nsec));
            TRY_SCOPED(co_await write_int<u32>(sock, stat.mode));
            TRY_SCOPED(co_await write_int<u32>(sock, stat.uid));
            TRY_SCOPED(co_await write_int<u32>(sock, stat.gid));
        }

        co_return Expect<void>{};
    }

    AExpect<void> write_resp_stat(Socket& sock, resp::Stat&& resp)
    {
        TRY_SCOPED(co_await write_int<i64>(sock, resp.size));
        TRY_SCOPED(co_await write_int<u64>(sock, resp.links));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.mtime.tv_sec));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.mtime.tv_nsec));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.atime.tv_sec));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.atime.tv_nsec));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.ctime.tv_sec));
        TRY_SCOPED(co_await write_int<i64>(sock, resp.ctime.tv_nsec));
        TRY_SCOPED(co_await write_int<u32>(sock, resp.mode));
        TRY_SCOPED(co_await write_int<u32>(sock, resp.uid));
        co_return co_await write_int<u32>(sock, resp.gid);
    }

    AExpect<void> write_resp_readlink(Socket& sock, resp::Readlink&& resp)
    {
        return write_path(sock, resp.target);
    }

    AExpect<void> write_resp_read(Socket& sock, resp::Read&& resp)
    {
        return write_bytes(sock, resp.read);
    }

    AExpect<void> write_resp_write(Socket& sock, resp::Write&& resp)
    {
        return write_int<u64>(sock, resp.size);
    }

    AExpect<void> write_resp_copy_file_range(Socket& sock, resp::CopyFileRange&& resp)
    {
        return write_int<u64>(sock, resp.size);
    }

    // --------------------

    AExpect<Procedure> Server::peek_req()
    {
        return read_procedure(m_socket);
    }

    AExpect<void> Server::send_resp(Var<Status, Response> response)
    {
        auto status = Status::Success;
        if (auto err = std::get_if<Status>(&response); err) {
            status = *err;
        }

        TRY_SCOPED(co_await write_status(m_socket, status));

        if (status != Status::Success) {
            co_return Expect<void>{};
        }

        auto empty_response = [&] -> AExpect<void> { co_return Expect<void>{}; };

        auto overload = util::Overload{
            // clang-format off
            [&](resp::Listdir&&       resp) { return write_resp_listdir        (m_socket, std::move(resp)); },
            [&](resp::Stat&&          resp) { return write_resp_stat           (m_socket, std::move(resp)); },
            [&](resp::Readlink&&      resp) { return write_resp_readlink       (m_socket, std::move(resp)); },
            [&](resp::Mknod&&             ) { return empty_response();                                      },
            [&](resp::Mkdir&&             ) { return empty_response();                                      },
            [&](resp::Unlink&&            ) { return empty_response();                                      },
            [&](resp::Rmdir&&             ) { return empty_response();                                      },
            [&](resp::Rename&&            ) { return empty_response();                                      },
            [&](resp::Truncate&&          ) { return empty_response();                                      },
            [&](resp::Read&&          resp) { return write_resp_read           (m_socket, std::move(resp)); },
            [&](resp::Write&&         resp) { return write_resp_write          (m_socket, std::move(resp)); },
            [&](resp::Utimens&&           ) { return empty_response();                                      },
            [&](resp::CopyFileRange&& resp) { return write_resp_copy_file_range(m_socket, std::move(resp)); },
            // clang-format on
        };

        co_return co_await std::visit(std::move(overload), std::get<Response>(std::move(response)));
    }
}

namespace madbfs::rpc
{
    Str to_string(Procedure procedure)
    {
        switch (procedure) {

        case Procedure::Listdir: return "Listdir";
        case Procedure::Stat: return "Stat";
        case Procedure::Readlink: return "Readlink";
        case Procedure::Mknod: return "Mknod";
        case Procedure::Mkdir: return "Mkdir";
        case Procedure::Unlink: return "Unlink";
        case Procedure::Rmdir: return "Rmdir";
        case Procedure::Rename: return "Rename";
        case Procedure::Truncate: return "Truncate";
        case Procedure::Read: return "Read";
        case Procedure::Write: return "Write";
        case Procedure::Utimens: return "Utimens";
        case Procedure::CopyFileRange: return "CopyFileRange";
        }

        return "Unknown";
    }

    Str to_string(Request request)
    {
        auto index = request.index() + 1;
        return to_string(static_cast<Procedure>(index));
    }

    Str to_string(Response response)
    {
        auto index = response.index() + 1;
        return to_string(static_cast<Procedure>(index));
    }
}
