#include "madbfs/data/cache.hpp"

#include "madbfs/connection/connection.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/defer.hpp>

namespace
{
    auto scoped_increment(madbfs::i64& counter)
    {
        ++counter;
        return madbfs::util::defer([&] { --counter; });
    }
}

namespace madbfs::data
{
    Page::Page(PageKey key, Uniq<char[]> buf, u32 size, u32 page_size)
        : m_key{ key }
        , m_data{ std::move(buf) }
        , m_size{ size }
        , m_page_size{ page_size }
    {
    }

    usize Page::read(Span<char> out, usize offset)
    {
        auto size = std::min(m_size - offset, out.size());
        std::copy_n(m_data.get() + offset, size, out.data());
        return size;
    }

    usize Page::write(Span<const char> in, usize offset)
    {
        if (offset >= m_page_size) [[unlikely]] {
            // NOTE: getting here is a bug in implementation
            log_c("{}: [BUG] offset exceed page size [{} vs {}]", __func__, offset, m_page_size);
            return 0;
        }

        auto end = static_cast<u32>(offset + in.size());
        if (end > m_page_size) [[unlikely]] {
            // NOTE: getting here is a bug in implementation
            log_c("{}: [BUG] offset + size exceed page size [{} vs {}]", __func__, end, m_page_size);
            end = std::min(end, m_page_size);
        }

        std::copy_n(in.data(), end - offset, m_data.get() + offset);
        m_size = std::max(end, m_size);

        return end - offset;
    }

    usize Page::truncate(usize size)
    {
        return m_size = std::min(static_cast<u32>(size), m_page_size);
    }

    usize Page::size() const
    {
        return m_size;
    }

    bool Page::is_dirty() const
    {
        return m_dirty;
    }

    void Page::set_dirty(bool set)
    {
        m_dirty = set;
    }
}

namespace madbfs::data
{
    Cache::Cache(async::Context& ctx, connection::Connection& connection, usize page_size, usize max_pages)
        : m_connection{ connection }
        , m_stale_fds_timer{ ctx }
        , m_page_size{ std::bit_ceil(page_size) }
        , m_max_pages{ max_pages }
    {
        async::spawn(ctx, reaper(), [](std::exception_ptr e) { log::log_exception(e, "reaper"); });
    }

    AExpect<void> Cache::hint_open(Id id, path::Path path, data::OpenMode mode)
    {
        // only adding new entry, actual open will be performed on read/write
        log_d("{}: [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));

        auto& entry = new_lookup(id, path).get();
        if (entry.path.str() != path.str()) {
            log_e("{}: path differs: old={:?} | new={:?}", __func__, entry.path, path);
            co_return Unexpect{ Errc::io_error };
        }

        const auto prev_reader = entry.reader;
        const auto prev_writer = entry.writer;

        entry.reader += mode == data::OpenMode::Read or mode == data::OpenMode::ReadWrite;
        entry.writer += mode == data::OpenMode::Write or mode == data::OpenMode::ReadWrite;

        // see note on reaper() function body regarding m_stale_fds

        if (prev_reader == 0 and entry.reader > 0) {
            log_t("{}: cancel stale [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));
            std::erase_if(m_stale_fds, [&](const auto& v) { return v == Tup{ id, FdKind::Read }; });
        }
        if (prev_writer == 0 and entry.writer > 0) {
            log_t("{}: cancel stale [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));
            std::erase_if(m_stale_fds, [&](const auto& v) { return v == Tup{ id, FdKind::Write }; });
        }

        co_return Expect<void>{};
    }

    AExpect<void> Cache::hint_close(Id id, data::OpenMode mode)
    {
        // only mark id's fd as stale on empty reader/writer, actual close performed on reaper()
        log_d("{}: [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));

        auto may_entry = lookup(id);
        if (not may_entry) {
            log_e("{}: hint_close [{}] is requested but no entry (forgot to open?)", __func__, id.inner());
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto& entry = may_entry->get();

        auto reader_decr = mode == data::OpenMode::Read or mode == data::OpenMode::ReadWrite;
        auto writer_decr = mode == data::OpenMode::Write or mode == data::OpenMode::ReadWrite;

        if ((reader_decr and entry.reader == 0) or (writer_decr and entry.writer == 0)) {
            log_e("{}: [{}] closed too many times", __func__, id.inner());
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        entry.reader -= reader_decr;
        entry.writer -= writer_decr;

        // see note on reaper() function body regarding m_stale_fds

        if (entry.reader == 0 and entry.read_fd) {
            log_t("{}: mark stale [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));
            m_stale_fds.emplace_back(id, FdKind::Read);
        }
        if (entry.writer == 0 and entry.write_fd) {
            log_t("{}: mark stale [id={}|mode={}]", __func__, id.inner(), std::to_underlying(mode));
            m_stale_fds.emplace_back(id, FdKind::Write);
        }

        co_return Expect<void>{};
    }

    AExpect<usize> Cache::read(Id id, Span<char> out, off_t offset)
    {
        auto first = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_page_size;

        log_d("{}: start [id={}|idx={} - {}]", __func__, id.inner(), first, last);

        auto entry = lookup(id);
        if (not entry) {
            log_e("{}: read [{}] is requested but no entry (forgot to open?)", __func__, id.inner());
            co_return Unexpect{ Errc::bad_file_descriptor };
        }

        auto work = [&](usize idx) { return read_at(entry->get(), out, id, idx, first, last, offset); };
        auto res  = co_await async::wait_all(sv::iota(first, last + 1) | sv::transform(work));

        auto read = 0uz;
        for (auto&& res : res) {
            if (not res) {
                log_e("{}: failed to read [{}]: {}", __func__, id.inner(), err_msg(res.error()));
                co_return Unexpect{ res.error() };
            }
            read += res.value();
        }

        co_return read;
    }

    AExpect<usize> Cache::write(Id id, Span<const char> in, off_t offset)
    {
        auto first = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_page_size;

        log_d("{}: start [id={}|idx={} - {}]", __func__, id.inner(), first, last);

        auto entry = lookup(id);
        if (not entry) {
            log_e("{}: read [{}] is requested but no entry (forgot to open?)", __func__, id.inner());
            co_return Unexpect{ Errc::bad_file_descriptor };
        }
        entry->get().dirty = true;

        auto work = [&](usize idx) { return write_at(entry->get(), in, id, idx, first, last, offset); };
        auto res  = co_await async::wait_all(sv::iota(first, last + 1) | sv::transform(work));

        auto written = 0uz;
        for (auto&& res : res) {
            if (not res) {
                log_e("{}: failed to write [{}]: {}", __func__, id.inner(), err_msg(res.error()));
                co_return Unexpect{ res.error() };
            }
            written += res.value();
        }

        co_return written;
    }

    AExpect<void> Cache::flush(Id id)
    {
        auto entry = lookup(id);
        if (not entry) {
            co_return Expect<void>{};
        }

        if (not entry->get().dirty) {
            co_return Expect<void>{};
        }

        const auto& pages = entry->get().pages;
        log_d("flush: start [id={}|idx={}]", id.inner(), pages | sv::keys);

        if (auto& e = entry->get(); not e.write_fd) {
            auto fd = co_await m_connection.open(e.path, data::OpenMode::Write);
            if (not fd) {
                co_return Unexpect{ fd.error() };
            }
            e.write_fd = *fd;
        }

        auto write_incr_lock = scoped_increment(entry->get().write_inflight);

        for (auto page : pages | sv::values) {
            if (not page->is_dirty()) {
                continue;
            }
            auto res = co_await flush_at(*entry->get().write_fd, *page, id);
            if (not res) {
                log_e("{}: failed to flush [{}]: {}", __func__, id.inner(), err_msg(res.error()));
                co_return Unexpect{ res.error() };
            }
        }

        entry->get().dirty = false;

        co_return Expect<void>{};
    }

    AExpect<void> Cache::truncate(Id id, usize old_size, usize new_size)
    {
        auto may_entry = lookup(id);
        if (not may_entry) {
            co_return Expect<void>{};
        }
        auto& entry = may_entry->get();

        auto old_num_pages = old_size / m_page_size + (old_size % m_page_size != 0);
        auto new_num_pages = new_size / m_page_size + (new_size % m_page_size != 0);

        auto num_pages = std::max(old_num_pages, new_num_pages);
        auto off_pages = std::max(1uz, std::min(old_num_pages, new_num_pages)) - 1;

        log_d(
            { "{}: start [id={}|idx={} - {}|old_pages={}|new_pages={}]" },
            __func__,
            id.inner(),
            off_pages,
            num_pages - 1,
            old_num_pages,
            new_num_pages
        );

        auto page_it = entry.pages.begin();

        if (new_num_pages > old_num_pages) {
            auto diff = new_num_pages - old_num_pages;
            if (m_lru.size() + diff > m_max_pages) {
                co_await evict(m_lru.size() + diff - m_max_pages);
            }
        }

        while (page_it != entry.pages.end()) {
            auto [index, page] = *page_it;
            if (index < off_pages or index >= num_pages) {
                ++page_it;
                continue;
            }

            log_t("{}: [id={}|idx={}]", __func__, id.inner(), index);

            auto key = PageKey{ id, index };
            if (index < old_num_pages - 1) {    // shrink
                m_lru.erase(page);
                page_it = entry.pages.erase(page_it);
            } else if (index > old_num_pages - 1) {    // grow
                auto rem_size = new_size - index * m_page_size;
                if (rem_size > m_page_size) {
                    rem_size = m_page_size;
                }
                m_lru.emplace_front(key, std::make_unique<char[]>(m_page_size), rem_size, m_page_size);
                entry.pages.emplace(index, m_lru.begin());
                ++page_it;
            } else {
                if (index == new_num_pages - 1) {
                    page->truncate((new_size - 1) % m_page_size + 1);
                } else {
                    page->truncate(m_page_size);
                }
                ++page_it;
            }
        }

        co_return Expect<void>{};
    }

    Await<void> Cache::rename(Id id, path::Path new_name)
    {
        if (auto found = m_table.find(id); found != m_table.end()) {
            found->second.path = new_name.owned();
        }
        co_return;
    }

    Await<void> Cache::invalidate_one(Id id, bool should_flush)
    {
        log_i("{}: invalidate one: {}", __func__, id.inner());

        if (should_flush) {
            if (auto res = co_await flush(id); not res) {
                log_e("{}: failed to flush {}: {}", __func__, id.inner(), err_msg(res.error()));
            }
        }

        if (auto entry = m_table.extract(id); not entry.empty()) {
            if (entry.mapped().dirty and not should_flush) {
                log_w("{}: [{}] is dirty but invalidated without flush!", __func__, id.inner());
            }
            for (auto page : entry.mapped().pages | sv::values) {
                m_lru.erase(page);
            }
        }
    }

    Await<void> Cache::invalidate_all()
    {
        co_await shutdown();

        auto exec = co_await async::current_executor();
        async::spawn(exec, reaper(), [](std::exception_ptr e) { log::log_exception(e, "reaper"); });

        log_i("{}: cache invalidated", __func__);
    }

    Await<void> Cache::shutdown()
    {
        m_read_queue.clear();
        m_stale_fds_timer.cancel();

        for (auto id : m_table | sv::keys) {
            if (auto res = co_await flush(id); not res) {
                log_e("{}: failed to flush {}: {}", __func__, id.inner(), err_msg(res.error()));
            }
        }

        for (auto& entry : m_table | sv::values) {
            if (entry.read_fd) {
                const auto fd = *entry.read_fd;
                entry.read_fd.reset();
                if (auto res = co_await m_connection.close(fd); not res) {
                    log_w("{}: failure on closing fd [{}]: {}", __func__, fd, err_msg(res.error()));
                }
            }
            if (entry.write_fd) {
                const auto fd = *entry.write_fd;
                entry.write_fd.reset();
                if (auto res = co_await m_connection.close(fd); not res) {
                    log_w("{}: failure on closing fd [{}]: {}", __func__, fd, err_msg(res.error()));
                }
            }
        }

        m_table.clear();
        m_lru.clear();
    }

    Await<void> Cache::set_page_size(usize new_page_size)
    {
        co_await shutdown();
        m_page_size = new_page_size;
        log_i("{}: page size changed to: {}", __func__, new_page_size);
    }

    Await<void> Cache::set_max_pages(usize new_max_pages)
    {
        co_await shutdown();
        m_max_pages = new_max_pages;
        log_i("{}: max pages can be stored changed to: {}", __func__, new_max_pages);
    }

    Ref<Cache::LookupEntry> Cache::new_lookup(Id id, path::Path path)
    {
        auto [it, _] = m_table.emplace(id, LookupEntry{ .pages = {}, .path = path.owned() });
        return std::ref(it->second);
    }

    // NOTE: std::unordered_map guarantees reference of its element valid even if new value inserted
    // (path parameter not nullopt)
    Opt<Ref<Cache::LookupEntry>> Cache::lookup(Id id)
    {
        if (auto entries = m_table.find(id); entries != m_table.end()) {
            return entries->second;
        }
        return std::nullopt;
    }

    AExpect<usize> Cache::on_miss(u64 fd, Span<char> out, off_t offset)
    {
        return m_connection.read(fd, out, offset);
    }

    AExpect<usize> Cache::on_flush(u64 fd, Span<const char> in, off_t offset)
    {
        return m_connection.write(fd, in, offset);
    }

    Await<void> Cache::evict(usize size)
    {
        while (size-- > 0 and not m_lru.empty()) {
            auto page      = std::move(m_lru.back());
            auto [id, idx] = page.key();

            m_lru.pop_back();

            auto entry = lookup(id);
            if (not entry) {
                log_c("{}: evict [id={}|idx={}] requested but no entry", __func__, id.inner(), idx);
                continue;
            }

            if (page.is_dirty()) {
                log_i("{}: force push page [id={}|idx={}]", __func__, id.inner(), idx);

                auto write_incr_lock = scoped_increment(entry->get().write_inflight);

                if (auto& e = entry->get(); not e.write_fd) {
                    auto fd = co_await m_connection.open(e.path, data::OpenMode::Write);
                    if (not fd) {
                        log_c("{}: force push [id={}|idx={}] can't open file", __func__, id.inner(), idx);
                        continue;
                    }
                    e.write_fd = *fd;
                }

                auto offset = static_cast<off_t>(idx * m_page_size);
                if (auto res = co_await on_flush(*entry->get().write_fd, page.buf(), offset); not res) {
                    log_c("{}: failed to force push page [id={}|idx={}]", __func__, id.inner(), idx);
                }
            }

            // this is done last since on_flush requires entry to still exists
            entry->get().pages.erase(idx);
        }
    }

    AExpect<usize> Cache::read_at(
        LookupEntry& entry,
        Span<char>   out,
        Id           id,
        usize        index,
        usize        first,
        usize        last,
        off_t        offset
    )
    {
        auto read_incr_lock = scoped_increment(entry.read_inflight);

        log_t("read: [id={}|idx={}]", id.inner(), index);

        auto key = PageKey{ id, index };

        if (auto queued = m_read_queue.find(key); queued != m_read_queue.end()) {
            auto fut = queued->second;
            co_await fut.async_wait();
            if (auto err = fut.get(); static_cast<bool>(err)) {
                co_return Unexpect{ err };
            }
        }

        auto page_entry = entry.pages.find(index);
        if (page_entry == entry.pages.end()) {
            // cache miss
            if (not entry.read_fd) {
                auto fd = co_await m_connection.open(entry.path, data::OpenMode::Read);
                if (not fd) {
                    co_return Unexpect{ fd.error() };
                }
                entry.read_fd = *fd;
            }

            auto promise = saf::promise<Errc>{ co_await async::current_executor() };
            auto future  = promise.get_future().share();
            m_read_queue.emplace(key, std::move(future));

            auto data    = std::make_unique<char[]>(m_page_size);
            auto span    = Span{ data.get(), m_page_size };
            auto may_len = co_await on_miss(*entry.read_fd, span, static_cast<off_t>(index * m_page_size));
            if (not may_len) {
                promise.set_value(may_len.error());
                m_read_queue.erase(key);
                co_return Unexpect{ may_len.error() };
            } else if (not m_read_queue.contains(key)) {
                promise.set_value(Errc::operation_canceled);
                co_return Unexpect{ Errc::operation_canceled };
            }

            m_lru.emplace_front(key, std::move(data), *may_len, m_page_size);
            auto [p, _] = entry.pages.emplace(index, m_lru.begin());
            page_entry  = p;

            promise.set_value(Errc{});
            m_read_queue.erase(key);

            if (m_lru.size() > m_max_pages) {
                co_await evict(m_lru.size() - m_max_pages);
            }
        }

        auto [_, page] = *page_entry;

        if (page != m_lru.begin()) {
            m_lru.splice(m_lru.begin(), m_lru, page);
        }

        auto local_offset = 0uz;
        auto local_size   = m_page_size;

        if (index == first) {
            local_offset = static_cast<usize>(offset) % m_page_size;
            local_size   = local_size - local_offset;
        }

        if (index == last) {
            auto off    = static_cast<usize>(offset) % m_page_size;
            local_size  = (out.size() + off - 1) % m_page_size + 1;
            local_size -= local_offset;
        }

        auto out_off = 0uz;
        if (index >= first + 1) {
            out_off = (index - first) * m_page_size - static_cast<usize>(offset) % m_page_size;
        }

        auto out_span = Span{ out.data() + out_off, local_size };
        auto read     = page->read(out_span, local_offset);

        co_return read;
    }

    AExpect<usize> Cache::write_at(
        LookupEntry&     entry,
        Span<const char> in,
        Id               id,
        usize            index,
        usize            first,
        usize            last,
        off_t            offset
    )
    {
        log_t("write: [id={}|idx={}]", id.inner(), index);

        auto key = PageKey{ id, index };

        if (auto queued = m_read_queue.find(key); queued != m_read_queue.end()) {
            auto fut = queued->second;
            co_await fut.async_wait();
            if (auto err = fut.get(); static_cast<bool>(err)) {
                co_return Unexpect{ err };
            }
        }

        auto page_entry = entry.pages.find(index);
        if (page_entry == entry.pages.end()) {
            m_lru.emplace_front(key, std::make_unique<char[]>(m_page_size), 0, m_page_size);
            auto [p, _] = entry.pages.emplace(index, m_lru.begin());
            page_entry  = p;

            if (m_lru.size() > m_max_pages) {
                co_await evict(m_lru.size() - m_max_pages);
            }
        }

        auto [_, page] = *page_entry;

        if (page != m_lru.begin()) {
            m_lru.splice(m_lru.begin(), m_lru, page);
        }

        auto local_offset = 0uz;
        auto local_size   = m_page_size;

        if (index == first) {
            local_offset = static_cast<usize>(offset) % m_page_size;
            local_size   = local_size - local_offset;
        }

        if (index == last) {
            auto off    = static_cast<usize>(offset) % m_page_size;
            local_size  = (in.size() + off - 1) % m_page_size + 1;
            local_size -= local_offset;
        }

        auto in_off = 0uz;
        if (index >= first + 1) {
            in_off = (index - first) * m_page_size - static_cast<usize>(offset) % m_page_size;
        }

        auto in_span = Span{ in.data() + in_off, local_size };
        auto written = page->write(in_span, local_offset);

        page->set_dirty(true);

        co_return written;
    }

    AExpect<void> Cache::flush_at(u64 fd, Page& page, Id id)
    {
        log_t("flush: [id={}|idx={}]", id.inner(), page.key().index);

        if (auto queued = m_read_queue.find(page.key()); queued != m_read_queue.end()) {
            auto fut = queued->second;
            co_await fut.async_wait();
            if (auto err = fut.get(); static_cast<bool>(err)) {
                co_return Unexpect{ err };
            }
        }

        auto data = std::make_unique<char[]>(m_page_size);
        auto read = page.read({ data.get(), m_page_size }, 0);
        page.set_dirty(false);

        auto span = Span{ data.get(), read };
        auto res  = co_await on_flush(fd, span, static_cast<off_t>(page.key().index * m_page_size));
        if (not res) {
            co_return Unexpect{ res.error() };
        }

        co_return Expect<void>{};
    }

    Await<void> Cache::reaper()
    {
        using namespace std::chrono_literals;
        constexpr auto interval = 10s;

        auto finished_fds    = std::vector<u64>{};
        auto stale_to_remove = std::vector<u64>{};

        while (true) {
            m_stale_fds_timer.expires_after(interval);
            auto res = co_await m_stale_fds_timer.async_wait();
            if (not res) {
                co_return;
            }

            log_d("{}: start erasing stale fds [count={}]", __func__, m_stale_fds.size());

            // NOTE: m_stale_fds must not be operated on between yielding points, else the data might be not
            // synchronized

            for (auto i : sv::iota(0uz, m_stale_fds.size())) {
                auto [id, kind] = m_stale_fds[i];

                auto lookup = m_table.find(id);
                if (lookup == m_table.end()) {
                    stale_to_remove.push_back(i);
                    continue;
                }

                auto& entry = lookup->second;
                auto  fd    = Opt<u64>{};

                switch (kind) {
                case FdKind::Read: entry.read_inflight == 0 ? entry.read_fd.swap(fd) : void(); break;
                case FdKind::Write: entry.write_inflight == 0 ? entry.write_fd.swap(fd) : void(); break;
                }

                if (fd) {
                    finished_fds.emplace_back(*fd);
                    stale_to_remove.push_back(i);
                }
            }

            for (auto i : stale_to_remove | sv::reverse) {
                m_stale_fds.erase(m_stale_fds.begin() + static_cast<isize>(i));
            }
            stale_to_remove.clear();

            // >> yielding point
            for (auto fd : finished_fds) {
                if (auto res = co_await m_connection.close(fd); not res) {
                    log_w("{}: failure on closing fd [{}]: {}", __func__, fd, err_msg(res.error()));
                }
            }

            finished_fds.clear();

            log_d("{}: finish erasing stale fds [count={}]", __func__, stale_to_remove.size());

            for (auto it = m_table.begin(); it != m_table.end();) {
                if (it->second.is_free()) {
                    const auto& [id, entry] = *it;
                    log_t("{}: remove free entry for [{}] {:?}", __func__, id.inner(), entry.path);
                    it = m_table.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}
