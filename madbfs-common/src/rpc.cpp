#include "madbfs-common/rpc.hpp"
#include "madbfs-common/async/async.hpp"
#include "madbfs-common/util/overload.hpp"

// TODO: add log
#define HANDLE_ERROR(ec, size, want)                                                                         \
    if (ec) {                                                                                                \
        co_return madbfs::Unexpect{ madbfs::async::to_generic_err(ec) };                                     \
    } else if (size != want) {                                                                               \
        co_return madbfs::Unexpect{ madbfs::Errc::broken_pipe };                                             \
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

        if constexpr (std::endian::native == std::endian::big) {
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

        if constexpr (std::endian::native == std::endian::big) {
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

    AExpect<void> write_bytes(Socket& sock, Span<const u8> bytes)
    {
        if (auto res = co_await write_int<u64>(sock, bytes.size()); not res) {
            co_return Unexpect{ res.error() };
        }
        auto [ec, size] = co_await madbfs::async::write_exact<u8>(sock, bytes);
        HANDLE_ERROR(ec, size, bytes.size());
        co_return Expect<void>{};
    }

    AExpect<void> write_path(Socket& sock, Str path)
    {
        if (auto res = co_await write_int<u64>(sock, path.size()); not res) {
            co_return Unexpect{ res.error() };
        }
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

    AExpect<Slice> read_bytes(Socket& sock, Vec<u8>& buf)
    {
        auto size = co_await read_int<u64>(sock);
        if (not size) {
            co_return Unexpect{ size.error() };
        }
        auto off = static_cast<isize>(buf.size());
        buf.resize(buf.size() + *size);
        auto out = Span{ buf.begin() + off, *size };

        auto [ec, n] = co_await async::read_exact<u8>(sock, out);
        HANDLE_ERROR(ec, n, size);
        co_return Slice{ off, n };
    }
}

namespace madbfs::rpc
{
    // client send request
    // -------------------

    AExpect<void> write_req_listdir(Socket& sock, const req::Listdir& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_stat(Socket& sock, const req::Stat& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_readlink(Socket& sock, const req::Readlink& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_mknod(Socket& sock, const req::Mknod& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_mkdir(Socket& sock, const req::Mkdir& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_unlink(Socket& sock, const req::Unlink& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_rmdir(Socket& sock, const req::Rmdir& req)
    {
        return write_path(sock, req.path);
    }

    AExpect<void> write_req_rename(Socket& sock, const req::Rename& req)
    {
        if (auto res = co_await write_path(sock, req.from); not res) {
            co_return res;
        }
        co_return co_await write_path(sock, req.to);
    }

    AExpect<void> write_req_truncate(Socket& sock, const req::Truncate& req)
    {
        if (auto res = co_await write_path(sock, req.path); not res) {
            co_return res;
        }
        co_return co_await write_int<i64>(sock, req.offset);
    }

    AExpect<void> write_req_read(Socket& sock, const req::Read& req)
    {
        if (auto res = co_await write_path(sock, req.path); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.offset); not res) {
            co_return res;
        }
        co_return co_await write_int<u64>(sock, req.size);
    }

    AExpect<void> write_req_write(Socket& sock, const req::Write& req)
    {
        if (auto res = co_await write_path(sock, req.path); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.offset); not res) {
            co_return res;
        }
        co_return co_await write_bytes(sock, req.in);
    }

    AExpect<void> write_req_utimens(Socket& sock, const req::Utimens& req)
    {
        if (auto res = co_await write_path(sock, req.path); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.atime.tv_sec); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.atime.tv_nsec); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.mtime.tv_sec); not res) {
            co_return res;
        }
        co_return co_await write_int<i64>(sock, req.mtime.tv_nsec);
    }

    AExpect<void> write_req_copy_file_range(Socket& sock, const req::CopyFileRange& req)
    {
        if (auto res = co_await write_path(sock, req.in_path); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.in_offset); not res) {
            co_return res;
        }
        if (auto res = co_await write_path(sock, req.out_path); not res) {
            co_return res;
        }
        if (auto res = co_await write_int<i64>(sock, req.out_offset); not res) {
            co_return res;
        }
        co_return co_await write_int<u64>(sock, req.size);
    }

    // -------------------

    // client read response
    // --------------------

    AExpect<Response> read_resp_listdir(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Listdir{} };
    }

    AExpect<Response> read_resp_stat(Socket& sock, Vec<u8>& buf)
    {
        auto size = co_await read_int<i64>(sock);
        if (not size) {
            co_return Unexpect{ size.error() };
        }
        auto links = co_await read_int<u64>(sock);
        if (not links) {
            co_return Unexpect{ links.error() };
        }
        auto mtime_sec = co_await read_int<i64>(sock);
        if (not mtime_sec) {
            co_return Unexpect{ mtime_sec.error() };
        }
        auto mtime_nsec = co_await read_int<i64>(sock);
        if (not mtime_nsec) {
            co_return Unexpect{ mtime_nsec.error() };
        }
        auto atime_sec = co_await read_int<i64>(sock);
        if (not atime_sec) {
            co_return Unexpect{ atime_sec.error() };
        }
        auto atime_nsec = co_await read_int<i64>(sock);
        if (not atime_nsec) {
            co_return Unexpect{ atime_nsec.error() };
        }
        auto ctime_sec = co_await read_int<i64>(sock);
        if (not ctime_sec) {
            co_return Unexpect{ ctime_sec.error() };
        }
        auto ctime_nsec = co_await read_int<i64>(sock);
        if (not ctime_nsec) {
            co_return Unexpect{ ctime_nsec.error() };
        }
        auto mode = co_await read_int<u32>(sock);
        if (not mode) {
            co_return Unexpect{ mode.error() };
        }
        auto uid = co_await read_int<u32>(sock);
        if (not uid) {
            co_return Unexpect{ uid.error() };
        }
        auto gid = co_await read_int<u32>(sock);
        if (not gid) {
            co_return Unexpect{ gid.error() };
        }

        co_return Response{ resp::Stat{
            .size  = *size,
            .links = *links,
            .mtime = { .tv_sec = *mtime_sec, .tv_nsec = *mtime_nsec },
            .atime = { .tv_sec = *atime_sec, .tv_nsec = *atime_nsec },
            .ctime = { .tv_sec = *ctime_sec, .tv_nsec = *ctime_nsec },
            .mode  = *mode,
            .uid   = *uid,
            .gid   = *gid,
        } };
    }

    AExpect<Response> read_resp_readlink(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Response{ resp::Readlink{ .target = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Response> read_resp_mknod(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Mknod{} };
    }

    AExpect<Response> read_resp_mkdir(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Mkdir{} };
    }

    AExpect<Response> read_resp_unlink(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Unlink{} };
    }

    AExpect<Response> read_resp_rmdir(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Rmdir{} };
    }

    AExpect<Response> read_resp_rename(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Rename{} };
    }

    AExpect<Response> read_resp_truncate(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Truncate{} };
    }

    AExpect<Response> read_resp_read(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Response{ resp::Read{ .read = slice_bytes(buf, slice) } };
        });
    }

    AExpect<Response> read_resp_write(Socket& sock, Vec<u8>&)
    {
        co_return (co_await read_int<u64>(sock)).transform([&](u64 size) {
            return Response{ resp::Write{ .size = size } };
        });
    }

    AExpect<Response> read_resp_utimens(Socket&, Vec<u8>&)
    {
        co_return Response{ resp::Utimens{} };
    }

    AExpect<Response> read_resp_copy_file_range(Socket& sock, Vec<u8>&)
    {
        co_return (co_await read_int<u64>(sock)).transform([&](u64 size) {
            return Response{ resp::CopyFileRange{ .size = size } };
        });
    }

    // --------------------

    AExpect<void> Client::send_req_procedure(Procedure procedure)
    {
        return write_int<u8>(m_socket, static_cast<u8>(procedure));
    }

    AExpect<void> Client::send_req_param(Request request)
    {
        auto overload = util::Overload{
            // clang-format off
            [&](const req::Listdir&       req) { return write_req_listdir        (m_socket, req); },
            [&](const req::Stat&          req) { return write_req_stat           (m_socket, req); },
            [&](const req::Readlink&      req) { return write_req_readlink       (m_socket, req); },
            [&](const req::Mknod&         req) { return write_req_mknod          (m_socket, req); },
            [&](const req::Mkdir&         req) { return write_req_mkdir          (m_socket, req); },
            [&](const req::Unlink&        req) { return write_req_unlink         (m_socket, req); },
            [&](const req::Rmdir&         req) { return write_req_rmdir          (m_socket, req); },
            [&](const req::Rename&        req) { return write_req_rename         (m_socket, req); },
            [&](const req::Truncate&      req) { return write_req_truncate       (m_socket, req); },
            [&](const req::Read&          req) { return write_req_read           (m_socket, req); },
            [&](const req::Write&         req) { return write_req_write          (m_socket, req); },
            [&](const req::Utimens&       req) { return write_req_utimens        (m_socket, req); },
            [&](const req::CopyFileRange& req) { return write_req_copy_file_range(m_socket, req); },
            // clang-format on
        };
        return std::visit(std::move(overload), std::move(request));
    }

    AExpect<Response> Client::recv_resp_procedure(Procedure procedure)
    {
        auto status = co_await read_int<u8>(m_socket);
        if (not status) {
            co_return Unexpect{ status.error() };
        } else if (*status != 0) {
            co_return Unexpect{ static_cast<Errc>(*status) };
        }

        // clang-format off
        switch (procedure) {
        case Procedure::Listdir:       co_return co_await read_resp_listdir        (m_socket, m_buffer);
        case Procedure::Stat:          co_return co_await read_resp_stat           (m_socket, m_buffer);
        case Procedure::Readlink:      co_return co_await read_resp_readlink       (m_socket, m_buffer);
        case Procedure::Mknod:         co_return co_await read_resp_mknod          (m_socket, m_buffer);
        case Procedure::Mkdir:         co_return co_await read_resp_mkdir          (m_socket, m_buffer);
        case Procedure::Unlink:        co_return co_await read_resp_unlink         (m_socket, m_buffer);
        case Procedure::Rmdir:         co_return co_await read_resp_rmdir          (m_socket, m_buffer);
        case Procedure::Rename:        co_return co_await read_resp_rename         (m_socket, m_buffer);
        case Procedure::Truncate:      co_return co_await read_resp_truncate       (m_socket, m_buffer);
        case Procedure::Read:          co_return co_await read_resp_read           (m_socket, m_buffer);
        case Procedure::Write:         co_return co_await read_resp_write          (m_socket, m_buffer);
        case Procedure::Utimens:       co_return co_await read_resp_utimens        (m_socket, m_buffer);
        case Procedure::CopyFileRange: co_return co_await read_resp_copy_file_range(m_socket, m_buffer);
        }
        // clang-format on
    }
}

namespace madbfs::rpc
{
    // server read request
    // -------------------

    AExpect<Request> read_req_listdir(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Listdir{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_stat(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Stat{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_readlink(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Readlink{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_mknod(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Mknod{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_mkdir(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Mkdir{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_unlink(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Unlink{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_rmdir(Socket& sock, Vec<u8>& buf)
    {
        co_return (co_await read_bytes(sock, buf)).transform([&](Slice slice) {
            return Request{ req::Rmdir{ .path = slice_as_str(buf, slice) } };
        });
    }

    AExpect<Request> read_req_rename(Socket& sock, Vec<u8>& buf)
    {
        auto from_slice = co_await read_bytes(sock, buf);
        if (not from_slice) {
            co_return Unexpect{ from_slice.error() };
        }
        auto to_slice = co_await read_bytes(sock, buf);
        if (not to_slice) {
            co_return Unexpect{ to_slice.error() };
        }

        auto from = slice_as_str(buf, *from_slice);
        auto to   = slice_as_str(buf, *to_slice);

        co_return Request{ req::Rename{ .from = from, .to = to } };
    }

    AExpect<Request> read_req_truncate(Socket& sock, Vec<u8>& buf)
    {
        auto path_slice = co_await read_bytes(sock, buf);
        if (not path_slice) {
            co_return Unexpect{ path_slice.error() };
        }
        auto offset = co_await read_int<i64>(sock);
        if (not offset) {
            co_return Unexpect{ offset.error() };
        }

        auto path_str = slice_as_str(buf, *path_slice);
        co_return Request{ req::Truncate{ .path = path_str, .offset = *offset } };
    }

    AExpect<Request> read_req_read(Socket& sock, Vec<u8>& buf)
    {
        auto path_slice = co_await read_bytes(sock, buf);
        if (not path_slice) {
            co_return Unexpect{ path_slice.error() };
        }
        auto offset = co_await read_int<i64>(sock);
        if (not offset) {
            co_return Unexpect{ offset.error() };
        }
        auto size = co_await read_int<u64>(sock);
        if (not size) {
            co_return Unexpect{ size.error() };
        }

        auto path = slice_as_str(buf, *path_slice);
        co_return Request{ req::Read{ .path = path, .offset = *offset, .size = *size } };
    }

    AExpect<Request> read_req_write(Socket& sock, Vec<u8>& buf)
    {
        auto path_slice = co_await read_bytes(sock, buf);
        if (not path_slice) {
            co_return Unexpect{ path_slice.error() };
        }
        auto offset = co_await read_int<i64>(sock);
        if (not offset) {
            co_return Unexpect{ offset.error() };
        }
        auto bytes_slice = co_await read_bytes(sock, buf);
        if (not bytes_slice) {
            co_return Unexpect{ bytes_slice.error() };
        }

        auto path  = slice_as_str(buf, *path_slice);
        auto bytes = slice_bytes(buf, *bytes_slice);

        co_return Request{ req::Write{ .path = path, .offset = *offset, .in = bytes } };
    }

    AExpect<Request> read_req_utimens(Socket& sock, Vec<u8>& buf)
    {
        auto path_slice = co_await read_bytes(sock, buf);
        if (not path_slice) {
            co_return Unexpect{ path_slice.error() };
        }
        auto atime_sec_slice = co_await read_int<i64>(sock);
        if (not atime_sec_slice) {
            co_return Unexpect{ atime_sec_slice.error() };
        }
        auto atime_nsec_slice = co_await read_int<i64>(sock);
        if (not atime_nsec_slice) {
            co_return Unexpect{ atime_nsec_slice.error() };
        }
        auto mtime_sec_slice = co_await read_int<i64>(sock);
        if (not mtime_sec_slice) {
            co_return Unexpect{ mtime_sec_slice.error() };
        }
        auto mtime_nsec_slice = co_await read_int<i64>(sock);
        if (not mtime_nsec_slice) {
            co_return Unexpect{ mtime_nsec_slice.error() };
        }

        auto path = slice_as_str(buf, *path_slice);
        co_return Request{ req::Utimens{
            .path  = path,
            .atime = { .tv_sec = *atime_sec_slice, .tv_nsec = *atime_nsec_slice },
            .mtime = { .tv_sec = *mtime_sec_slice, .tv_nsec = *mtime_nsec_slice },
        } };
    }

    AExpect<Request> read_req_copy_file_range(Socket& sock, Vec<u8>& buf)
    {
        auto in_slice = co_await read_bytes(sock, buf);
        if (not in_slice) {
            co_return Unexpect{ in_slice.error() };
        }
        auto in_offset = co_await read_int<i64>(sock);
        if (not in_offset) {
            co_return Unexpect{ in_offset.error() };
        }
        auto out_slice = co_await read_bytes(sock, buf);
        if (not out_slice) {
            co_return Unexpect{ out_slice.error() };
        }
        auto out_offset = co_await read_int<i64>(sock);
        if (not out_offset) {
            co_return Unexpect{ out_offset.error() };
        }
        auto size = co_await read_int<u64>(sock);
        if (not size) {
            co_return Unexpect{ size.error() };
        }

        auto in  = slice_as_str(buf, *in_slice);
        auto out = slice_as_str(buf, *out_slice);

        co_return Request{ req::CopyFileRange{
            .in_path    = in,
            .in_offset  = *in_offset,
            .out_path   = out,
            .out_offset = *out_offset,
            .size       = *size,
        } };
    }

    // -------------------

    // server send response
    // --------------------

    AExpect<void> write_resp_listdir(Socket& sock, const resp::Listdir& resp)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_stat(Socket& sock, const resp::Stat& resp)
    {
        if (auto res = co_await write_int<i64>(sock, resp.size); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u64>(sock, resp.links); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.mtime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.mtime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.atime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.atime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.ctime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(sock, resp.ctime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u32>(sock, resp.mode); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u32>(sock, resp.uid); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return co_await write_int<u32>(sock, resp.gid);
    }

    AExpect<void> write_resp_readlink(Socket& sock, const resp::Readlink& resp)
    {
        return write_path(sock, resp.target);
    }

    AExpect<void> write_resp_mknod(Socket& sock, const resp::Mknod&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_mkdir(Socket& sock, const resp::Mkdir&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_unlink(Socket& sock, const resp::Unlink&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_rmdir(Socket& sock, const resp::Rmdir&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_rename(Socket& sock, const resp::Rename&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_truncate(Socket& sock, const resp::Truncate&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_read(Socket& sock, const resp::Read& resp)
    {
        return write_bytes(sock, resp.read);
    }

    AExpect<void> write_resp_write(Socket& sock, const resp::Write& resp)
    {
        return write_int<u64>(sock, resp.size);
    }

    AExpect<void> write_resp_utimens(Socket& sock, const resp::Utimens&)
    {
        co_return Expect<void>{};
    }

    AExpect<void> write_resp_copy_file_range(Socket& sock, const resp::CopyFileRange& resp)
    {
        return write_int<u64>(sock, resp.size);
    }

    // --------------------

    AExpect<Procedure> Server::recv_req_procedure()
    {
        auto byte = co_await read_int<u8>(m_socket);
        co_return byte.transform([](u8 b) { return static_cast<Procedure>(b); });
    }

    AExpect<Request> Server::recv_req_param(Procedure procedure)
    {
        // clang-format off
        switch (procedure) {
        case Procedure::Listdir:       return read_req_listdir        (m_socket, m_buffer);
        case Procedure::Stat:          return read_req_stat           (m_socket, m_buffer);
        case Procedure::Readlink:      return read_req_readlink       (m_socket, m_buffer);
        case Procedure::Mknod:         return read_req_mknod          (m_socket, m_buffer);
        case Procedure::Mkdir:         return read_req_mkdir          (m_socket, m_buffer);
        case Procedure::Unlink:        return read_req_unlink         (m_socket, m_buffer);
        case Procedure::Rmdir:         return read_req_rmdir          (m_socket, m_buffer);
        case Procedure::Rename:        return read_req_rename         (m_socket, m_buffer);
        case Procedure::Truncate:      return read_req_truncate       (m_socket, m_buffer);
        case Procedure::Read:          return read_req_read           (m_socket, m_buffer);
        case Procedure::Write:         return read_req_write          (m_socket, m_buffer);
        case Procedure::Utimens:       return read_req_utimens        (m_socket, m_buffer);
        case Procedure::CopyFileRange: return read_req_copy_file_range(m_socket, m_buffer);
        }
        // clang-format on
    }

    AExpect<void> Server::send_resp_procedure(Var<Status, Response> response)
    {
        auto status = static_cast<u8>(Status::Success);
        if (auto err = std::get_if<Status>(&response); err) {
            status = static_cast<u8>(*err);
        }

        if (auto res = co_await write_int<u8>(m_socket, status)) {
            co_return Unexpect{ res.error() };
        }

        if (status != 0) {
            co_return Expect<void>{};
        }

        auto overload = util::Overload{
            // clang-format off
            [&](const resp::Listdir&       resp) { return write_resp_listdir        (m_socket, resp); },
            [&](const resp::Stat&          resp) { return write_resp_stat           (m_socket, resp); },
            [&](const resp::Readlink&      resp) { return write_resp_readlink       (m_socket, resp); },
            [&](const resp::Mknod&         resp) { return write_resp_mknod          (m_socket, resp); },
            [&](const resp::Mkdir&         resp) { return write_resp_mkdir          (m_socket, resp); },
            [&](const resp::Unlink&        resp) { return write_resp_unlink         (m_socket, resp); },
            [&](const resp::Rmdir&         resp) { return write_resp_rmdir          (m_socket, resp); },
            [&](const resp::Rename&        resp) { return write_resp_rename         (m_socket, resp); },
            [&](const resp::Truncate&      resp) { return write_resp_truncate       (m_socket, resp); },
            [&](const resp::Read&          resp) { return write_resp_read           (m_socket, resp); },
            [&](const resp::Write&         resp) { return write_resp_write          (m_socket, resp); },
            [&](const resp::Utimens&       resp) { return write_resp_utimens        (m_socket, resp); },
            [&](const resp::CopyFileRange& resp) { return write_resp_copy_file_range(m_socket, resp); },
            // clang-format on
        };

        co_return co_await std::visit(std::move(overload), std::get<Response>(std::move(response)));
    }
}

namespace madbfs::rpc::listdir_channel
{
    AExpect<void> Sender::send_next(Dirent dirent)
    {
        if (auto res = co_await write_path(m_socket, dirent.name); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u64>(m_socket, dirent.links); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.size); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.mtime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.mtime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.atime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.atime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.ctime.tv_sec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<i64>(m_socket, dirent.ctime.tv_nsec); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u32>(m_socket, dirent.mode); not res) {
            co_return Unexpect{ res.error() };
        }
        if (auto res = co_await write_int<u32>(m_socket, dirent.uid); not res) {
            co_return Unexpect{ res.error() };
        }
        co_return co_await write_int<u32>(m_socket, dirent.gid);
    }

    AExpect<Dirent> Receiver::recv_next()
    {
        auto name_slice = co_await read_bytes(m_socket, m_buffer);
        if (not name_slice) {
            co_return Unexpect{ name_slice.error() };
        }
        auto links = co_await read_int<u64>(m_socket);
        if (not links) {
            co_return Unexpect{ links.error() };
        }
        auto size = co_await read_int<i64>(m_socket);
        if (not size) {
            co_return Unexpect{ size.error() };
        }
        auto mtime_sec = co_await read_int<i64>(m_socket);
        if (not mtime_sec) {
            co_return Unexpect{ mtime_sec.error() };
        }
        auto mtime_nsec = co_await read_int<i64>(m_socket);
        if (not mtime_nsec) {
            co_return Unexpect{ mtime_nsec.error() };
        }
        auto atime_sec = co_await read_int<i64>(m_socket);
        if (not atime_sec) {
            co_return Unexpect{ atime_sec.error() };
        }
        auto atime_nsec = co_await read_int<i64>(m_socket);
        if (not atime_nsec) {
            co_return Unexpect{ atime_nsec.error() };
        }
        auto ctime_sec = co_await read_int<i64>(m_socket);
        if (not ctime_sec) {
            co_return Unexpect{ ctime_sec.error() };
        }
        auto ctime_nsec = co_await read_int<i64>(m_socket);
        if (not ctime_nsec) {
            co_return Unexpect{ ctime_nsec.error() };
        }
        auto mode = co_await read_int<u32>(m_socket);
        if (not mode) {
            co_return Unexpect{ mode.error() };
        }
        auto uid = co_await read_int<u32>(m_socket);
        if (not uid) {
            co_return Unexpect{ uid.error() };
        }
        auto gid = co_await read_int<u32>(m_socket);
        if (not gid) {
            co_return Unexpect{ gid.error() };
        }

        co_return Dirent{
            .name  = slice_as_str(m_buffer, *name_slice),
            .links = *links,
            .size  = *size,
            .mtime = { .tv_sec = *mtime_sec, .tv_nsec = *mtime_nsec },
            .atime = { .tv_sec = *atime_sec, .tv_nsec = *atime_nsec },
            .ctime = { .tv_sec = *ctime_sec, .tv_nsec = *ctime_nsec },
            .mode  = *mode,
            .uid   = *uid,
            .gid   = *gid,
        };
    }
}
