#pragma once

#include "madbfs/madbfs.hpp"

#include <madbfs-common/async/async.hpp>
#include <madbfs-common/log.hpp>

namespace madbfs::ll
{
    class Bridge
    {
    public:
        Bridge(String mount_point, bool writeback)
            : m_mount_point{ mount_point }
            , m_writeback{ writeback }
        {
        }

        template <typename Ret, typename... Args>
        Ret invoke_fs(Await<Ret> (Filesystem::*fn)(Args...), std::type_identity_t<Args>... args) noexcept
        {
            try {
                auto& fs   = m_madbfs->fs();
                auto  coro = (fs.*fn)(std::forward<Args>(args)...);
                return async::block(*m_ctx, std::move(coro));
            } catch (const std::exception& e) {
                log_c(__func__, "exception occurred: {}", e.what());
            } catch (...) {
                log_c(__func__, "unknown exception occurred");
            }
            return Unexpect{ Errc::io_error };
        }

        void set_madbfs(Madbfs* madbfs) { m_madbfs = madbfs; }
        void set_ctx(async::Context* ctx) { m_ctx = ctx; }

        void set_writeback(bool enabled) { m_writeback = enabled; }
        bool writeback() const { return m_writeback; }

        Seconds ttl() const { return m_madbfs->ttl().value_or(Seconds::max()); }

        Str mount() const { return m_mount_point; }

    private:
        Madbfs*         m_madbfs      = nullptr;
        async::Context* m_ctx         = nullptr;
        String          m_mount_point = {};
        bool            m_writeback   = false;
    };
}
