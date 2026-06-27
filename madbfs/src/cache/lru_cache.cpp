#include "madbfs/cache/lru_cache.hpp"

#include "madbfs/cache/page.hpp"
#include "madbfs/connection.hpp"

#include <madbfs-common/log.hpp>
#include <madbfs-common/util/defer.hpp>
#include <madbfs-common/util/var_wrapper.hpp>

#include <saf.hpp>

#include <map>

using namespace madbfs;
using namespace madbfs::cache;

// helper functions/classes
namespace
{
    namespace op
    {
        // clang-format off
        struct HintOpen      { Id id; path::Path path; OpenMode mode; };
        struct HintClose     { Id id; OpenMode mode; };
        struct Read          { Id id; Span<char> out; off_t offset; };
        struct Write         { Id id; Span<const char> in; off_t offset; };
        struct Flush         { Id id; };
        struct Truncate      { Id id; usize old_size; usize new_size; };
        struct Rename        { Id id; path::Path new_name; };
        struct InvalidateOne { Id id; bool should_flush; };
        struct InvalidateAll { };
        struct InvalidateFds { bool close; };
        struct CleanStaleFds { };
        struct SetPageSize   { usize new_page_size; };
        struct SetMaxPages   { usize new_max_pages; };
        // clang-format on
    }

    /**
     * @class Op
     *
     * @brief Possible operations for LruCache.
     */
    struct Op    //
        : util::VarWrapper<
              op::HintOpen,
              op::HintClose,
              op::Read,
              op::Write,
              op::Flush,
              op::Truncate,
              op::Rename,
              op::InvalidateOne,
              op::InvalidateAll,
              op::InvalidateFds,
              op::CleanStaleFds,
              op::SetPageSize,
              op::SetMaxPages>
    {
        using VarWrapper::VarWrapper;
    };

    struct Work
    {
        Op                          op;
        saf::promise<Expect<usize>> result;
    };

    enum class FdKind
    {
        Read,
        Write,
    };

    struct FileEntry;

    using Channel     = async::Channel<Work>;
    using LookupTable = std::unordered_map<Id, FileEntry>;
    using BusyQueue   = std::unordered_map<PageKey, saf::shared_future<Errc>>;

    /**
     * @class LookupEntry
     *
     * @brief Entry for lookup table.
     *
     * Contains information of a file being cached (including all the cached pages).
     */
    struct FileEntry
    {
    public:
        std::map<usize, PageId> pages;
        path::PathBuf           path;

        u64 reader = 0;    // reader counts from FUSE
        u64 writer = 0;    // writer counts from FUSE

        Opt<u64> read_fd  = std::nullopt;    // real file descriptor for read (cache misses) on device
        Opt<u64> write_fd = std::nullopt;    // real file descriptor for write (dirty flushes) on device

        i64 read_inflight  = 0;    // read operation not completed yet on real fd
        i64 write_inflight = 0;    // write operation not completed yet on real fd

        bool dirty = false;

        /**
         * @brief Check if an entry is free to be discarded.
         */
        bool is_free() const
        {
            return pages.empty()          //
               and reader == 0            //
               and writer == 0            //
               and read_inflight == 0     //
               and write_inflight == 0    //
               and not dirty;
        }

        AExpect<u64> get_read_fd_or_open(Connection& connection)
        {
            if (not read_fd) {
                co_return (co_await connection.open(path, OpenMode::Read))    //
                    .transform([&](u64 fd) { return read_fd.emplace(fd); });
            }
            co_return *read_fd;
        }

        AExpect<u64> get_write_fd_or_open(Connection& connection)
        {
            if (not write_fd) {
                co_return (co_await connection.open(path, OpenMode::Write))    //
                    .transform([&](u64 fd) { return write_fd.emplace(fd); });
            }
            co_return *write_fd;
        }
    };

    /**
     * @brief Helper function that increments at call and decrement at destruction.
     *
     * @param counter Counter to increment/decrement.
     *
     * @return A deferred object.
     */
    util::Deferred auto scoped_increment(i64& counter)
    {
        ++counter;
        return util::defer([&] { --counter; });
    }
}

// lru_cache.hpp impl: LruCache::Impl
namespace madbfs::cache
{
    /**
     * @class LruCache::Impl
     *
     * @brief The implamentation of the LRU cache with queue system.
     *
     * The queue is needed to make sure that the operations order is preserved even when using async code. The
     * serialization is done using the same trick as `ProxyTransport` and `AdbTransport`, it uses Channel that
     * has its acceptor side be another detached coroutine.
     */
    class LruCache::Impl
    {
    public:
        Impl(net::any_io_executor exec, Connection& connection, usize page_size, usize max_pages)
            : m_connection{ connection }
            , m_channel{ exec }
            , m_pages{ static_cast<u32>(page_size), static_cast<u32>(max_pages) }
        {
        }

        ~Impl()
        {
            m_channel.cancel();
            m_channel.close();
        }

        /**
         * @brief Queue new operation to the channel.
         *
         * @param op Operation to be queued.
         */
        AExpect<usize> queue(Op op)
        {
            auto promise = saf::promise<Expect<usize>>{ co_await async::current_executor() };
            auto future  = promise.get_future();

            if (auto res = co_await m_channel.async_send({}, { op, std::move(promise) }); not res) {
                log_e(__func__, "failed to queue op: {}", res.error().message());
                co_return Unexpect{ async::to_generic_err(res.error(), Errc::broken_pipe) };
            }

            co_return co_await future.async_extract();
        }

        /**
         * @brief Initialize channel acceptor coroutine.
         */
        Await<void> initialize()
        {
            auto exec = co_await async::current_executor();
            async::spawn(exec, actor_loop(), [](std::exception_ptr e) {
                log::log_exception(e, "LruCache::Impl");
            });
        }

        /**
         * @brief Flush dirty pages and then shutdown channel.
         */
        Await<void> shutdown()
        {
            std::ignore = co_await queue(op::InvalidateAll{});
            m_channel.cancel();
            m_channel.close();
        }

        usize page_size() const { return m_pages.page_size(); }

        usize max_pages() const { return m_pages.max_pages(); }

        usize current_pages() const { return m_pages.count(); }

    private:
        /**
         * @brief Acceptor of the channel.
         *
         * All operations are execution from here.
         */
        Await<void> actor_loop()
        {
            while (true) {
                auto work = co_await m_channel.async_receive();
                if (not work) {
                    log_w(__func__, "failed to receive from channel: {}", work.error().message());
                    break;
                }

                auto& [op, promise] = *work;

                auto result = co_await op.visit(Overload{
                    // clang-format off
                    [&](op::HintOpen      op) { return handle_hint_open(op.id, op.path, op.mode);        },
                    [&](op::HintClose     op) { return handle_hint_close(op.id, op.mode);                },
                    [&](op::Read          op) { return handle_read(op.id, op.out, op.offset);            },
                    [&](op::Write         op) { return handle_write(op.id, op.in, op.offset);            },
                    [&](op::Flush         op) { return handle_flush(op.id);                              },
                    [&](op::Truncate      op) { return handle_truncate(op.id, op.old_size, op.new_size); },
                    [&](op::Rename        op) { return handle_rename(op.id, op.new_name);                },
                    [&](op::InvalidateOne op) { return handle_invalidate_one(op.id, op.should_flush);    },
                    [&](op::InvalidateAll   ) { return handle_invalidate_all();                          },
                    [&](op::InvalidateFds op) { return handle_invalidate_fds(op.close);                  },
                    [&](op::CleanStaleFds   ) { return handle_clean_stale_fds();                         },
                    [&](op::SetPageSize   op) { return handle_set_page_size(op.new_page_size);           },
                    [&](op::SetMaxPages   op) { return handle_set_max_pages(op.new_max_pages);           },
                    // clang-format on
                });

                promise.set_value(result);
            }
        }

        /**
         * @brief Hint the cache to open a new file descriptor.
         *
         * The file will only be opened when a read or write is performed the first time since `hint_open()`.
         * This function will only increase a reference counter of the reader or writer. Real file descriptor
         * for reading and writing is different. If a file reader/writer counter was zero before calling this,
         * the stale status of associated file descriptor will be cancelled (see `handle_hint_close()`).
         *
         * see: `LruCache::hint_open()`
         */
        AExpect<usize> handle_hint_open(Id id, path::Path path, OpenMode mode)
        {
            // only adding new entry, actual open will be performed on read/write
            log_d(__func__, "[id={}|mode={}] {:?}", id.inner(), std::to_underlying(mode), path);

            auto& entry = new_lookup_file(id, path);
            if (entry.path.str() != path.str()) {
                log_e(__func__, "path differs: old={:?} | new={:?}", entry.path, path);
                co_return Unexpect{ Errc::io_error };
            }

            const auto prev_reader = entry.reader;
            const auto prev_writer = entry.writer;

            entry.reader += mode == OpenMode::Read or mode == OpenMode::ReadWrite;
            entry.writer += mode == OpenMode::Write or mode == OpenMode::ReadWrite;

            // see note on clean_stale_fds() function body regarding m_stale_fds

            if (prev_reader == 0 and entry.reader > 0) {
                log_t(__func__, "cancel stale [id={}|mode={}]", id.inner(), std::to_underlying(mode));
                std::erase_if(m_stale_fds, [&](const auto& v) { return v == Tup{ id, FdKind::Read }; });
            }
            if (prev_writer == 0 and entry.writer > 0) {
                log_t(__func__, "cancel stale [id={}|mode={}]", id.inner(), std::to_underlying(mode));
                std::erase_if(m_stale_fds, [&](const auto& v) { return v == Tup{ id, FdKind::Write }; });
            }

            co_return 0;
        }

        /**
         * @brief Hint the cache to close a file descriptor.
         *
         * The function will only decrease the counter of the associated open mode of the file. If it reached
         * zero, the associated file descriptor will be marked as stale. Stale fds will be removed if the
         * function `LruCache::clean_stale_fds()` (in effect `handle_clean_stale_fds()`) is called.
         *
         * see: `LruCache::hint_close()`
         */
        AExpect<usize> handle_hint_close(Id id, OpenMode mode)
        {
            // only mark id's fd as stale on empty reader/writer, actual close performed on clean_stale_fds()
            log_d(__func__, "[id={}|mode={}]", id.inner(), std::to_underlying(mode));

            auto entry = lookup_file(id);
            if (not entry) {
                log_e(__func__, "hint_close [{}] is requested but no entry (forgot to open?)", id.inner());
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            auto reader_decr = mode == OpenMode::Read or mode == OpenMode::ReadWrite;
            auto writer_decr = mode == OpenMode::Write or mode == OpenMode::ReadWrite;

            if ((reader_decr and entry->reader == 0) or (writer_decr and entry->writer == 0)) {
                log_e(__func__, "[{}] closed too many times", id.inner());
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            entry->reader -= reader_decr;
            entry->writer -= writer_decr;

            // see note on clean_stale_fds() function body regarding m_stale_fds

            if (entry->reader == 0 and entry->read_fd) {
                log_t(__func__, "mark stale [id={}|mode={}]", id.inner(), std::to_underlying(mode));
                m_stale_fds.emplace_back(id, FdKind::Read);
            }
            if (entry->writer == 0 and entry->write_fd) {
                log_t(__func__, "mark stale [id={}|mode={}]", id.inner(), std::to_underlying(mode));
                m_stale_fds.emplace_back(id, FdKind::Write);
            }

            co_return 0;
        }

        /**
         * @brief Read from cache to output buffer.
         *
         * The function will perform a fetch from the device when necessary. Eviction will also happen if the
         * LRU is full to relinquish a new space for fetching data from device. This function only divides the
         * work into chunks, the real work is done on `read_at()`.
         *
         * see: `LruCache::read()`
         */
        AExpect<usize> handle_read(Id id, Span<char> out, off_t offset)
        {
            auto first = static_cast<usize>(offset) / m_pages.page_size();
            auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_pages.page_size();

            log_t(__func__, "start [id={}|idx={} - {}]", id.inner(), first, last);

            auto indices = sv::iota(first, last + 1);
            auto entry   = lookup_file(id);

            if (not entry) {
                log_e(__func__, "read [{}] is requested but no entry (forgot to open?)", id.inner());
                co_return Unexpect{ Errc::bad_file_descriptor };
            }

            // NOTE: To parallelize operation using `wait_all()`, all operation being performed must be safe
            // to be parallelized. For example, since the cache opens file lazily, `read_at()` might invoke
            // `open()` of `Connection`. The `open()` invocation must be done once, that is the reason for
            // this cache and it being lazy. But, because of this parallel invocation, the invocation can be
            // done more than once. Contract is not fulfilled. It will lead to "fd leak" on the device where
            // fd open and close is imbalanced. It will lead to EMFILE (24): Too many open files.

            // auto work = [&](usize idx) { return read_at(*entry, out, id, idx, first, last, offset); };
            // auto res  = co_await async::wait_all(indices | sv::transform(work));
            //
            // auto read = 0uz;
            // for (auto&& res : res) {
            //     if (not res) {
            //         log_e(__func__, "failed to read [{}]: {}", id.inner(), err_msg(res.error()));
            //         co_return Unexpect{ res.error() };
            //     }
            //     read += res.value();
            // }

            auto read = 0uz;
            for (auto index : indices) {
                auto res = co_await read_at(*entry, out, id, index, first, last, offset);
                if (not res) {
                    log_e(__func__, "failed to read [{}]: {}", id.inner(), err_msg(res.error()));
                    co_return Unexpect{ res.error() };
                } else {
                    read += res.value();
                }
            }

            co_return read;
        }

        /**
         * @brief Write to cache from input buffer.
         *
         * Writes are done to the LRU. Eviction may happen if the LRU is full. This function only divides the
         * work into chunks the real work is done on `write_at()`
         *
         * see: `LruCache::write()`
         */
        AExpect<usize> handle_write(Id id, Span<const char> in, off_t offset)
        {
            auto first = static_cast<usize>(offset) / m_pages.page_size();
            auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_pages.page_size();

            log_t(__func__, "start [id={}|idx={} - {}]", id.inner(), first, last);

            auto indices = sv::iota(first, last + 1);
            auto entry   = lookup_file(id);

            if (not entry) {
                log_e(__func__, "read [{}] is requested but no entry (forgot to open?)", id.inner());
                co_return Unexpect{ Errc::bad_file_descriptor };
            }
            entry->dirty = true;

            // NOTE: read note on `handle_read()`

            // auto work = [&](usize idx) { return write_at(*entry, in, id, idx, first, last, offset); };
            // auto res  = co_await async::wait_all(indices | sv::transform(work));
            //
            // auto written = 0uz;
            // for (auto&& res : res) {
            //     if (not res) {
            //         log_e(__func__, "failed to write [{}]: {}", id.inner(), err_msg(res.error()));
            //         co_return Unexpect{ res.error() };
            //     }
            //     written += res.value();
            // }

            auto written = 0uz;
            for (auto index : indices) {
                auto res = co_await write_at(*entry, in, id, index, first, last, offset);
                if (not res) {
                    log_e(__func__, "failed to write [{}]: {}", id.inner(), err_msg(res.error()));
                    co_return Unexpect{ res.error() };
                } else {
                    written += res.value();
                }
            }

            co_return written;
        }

        /**
         * @brief Flush dirty data into device.
         *
         * see: `LruCache::flush()`
         */
        AExpect<usize> handle_flush(Id id)
        {
            auto entry = lookup_file(id);

            if (not entry) {
                co_return 0;
            } else if (not entry->dirty) {
                co_return 0;
            }

            const auto& pages = entry->pages;
            auto        wlock = scoped_increment(entry->write_inflight);

            log_t(__func__, "flush: start [id={}|idx={}]", id.inner(), pages | sv::keys);

            auto fd = co_await entry->get_write_fd_or_open(m_connection);
            if (not fd) {
                co_return Unexpect{ fd.error() };
            }

            for (auto page_id : pages | sv::values) {
                if (auto [page, key] = m_pages.get(page_id, false); page.is_dirty()) {
                    if (auto res = co_await flush_at(*fd, page, key); not res) {
                        log_e(__func__, "failed to flush [{}]: {}", id.inner(), err_msg(res.error()));
                        co_return Unexpect{ res.error() };
                    }
                }
            }

            entry->dirty = false;
            co_return 0;
        }

        /**
         * @brief Truncate in-cache file data to specified size.
         *
         * The function won't touch the file on the device, that's the responsibility of the caller. It won't
         * mark affected pages as dirty.
         *
         * see: `LruCache::truncate()`
         */
        AExpect<usize> handle_truncate(Id id, usize old_size, usize new_size)
        {
            auto may_entry = lookup_file(id);
            if (not may_entry) {
                co_return 0;
            }
            auto& entry = *may_entry;

            auto old_num_pages = old_size / m_pages.page_size() + (old_size % m_pages.page_size() != 0);
            auto new_num_pages = new_size / m_pages.page_size() + (new_size % m_pages.page_size() != 0);

            auto num_pages = std::max(old_num_pages, new_num_pages);
            auto off_pages = std::max(1uz, std::min(old_num_pages, new_num_pages)) - 1;

            log_t(
                __func__,
                "start [id={}|idx={} - {}|old_pages={}|new_pages={}]",
                id.inner(),
                off_pages,
                num_pages - 1,
                old_num_pages,
                new_num_pages
            );

            auto pages_it = entry.pages.begin();

            while (pages_it != entry.pages.end()) {
                auto [index, page_id] = *pages_it;
                if (index < off_pages or index >= num_pages) {
                    ++pages_it;
                    continue;
                }

                log_t(__func__, "[id={}|idx={}]", id.inner(), index);

                if (index < old_num_pages - 1) {    // shrink
                    m_pages.release(page_id);
                    pages_it = entry.pages.erase(pages_it);
                } else if (index > old_num_pages - 1) {    // grow
                    auto rem_size = new_size - index * m_pages.page_size();
                    if (rem_size > m_pages.page_size()) {
                        rem_size = m_pages.page_size();
                    }

                    auto [page_id, page] = co_await acquire_or_evict(PageKey{ id, index });
                    entry.pages.emplace(index, page_id);
                    page.truncate(rem_size);

                    ++pages_it;
                } else {
                    if (index == new_num_pages - 1) {
                        m_pages.get(page_id, true).page.truncate((new_size - 1) % m_pages.page_size() + 1);
                    } else {
                        m_pages.get(page_id, true).page.truncate(m_pages.page_size());
                    }
                    ++pages_it;
                }
            }

            co_return 0;
        }

        /**
         * @brief Rename file path pointed by its id to new name.
         *
         * This function is necessary since the cache opens file descriptor lazily. This function never fails.
         *
         * see: `LruCache::rename()`
         */
        AExpect<usize> handle_rename(Id id, path::Path new_name)
        {
            if (auto found = m_table.find(id); found != m_table.end()) {
                found->second.path = new_name.owned();
            }
            co_return 0;
        }

        /**
         * @brief Invalidate entries for a single file by its id.
         *
         * This function never fails.
         *
         * see: `LruCache::invalidate_one()`
         */
        AExpect<usize> handle_invalidate_one(Id id, bool should_flush)
        {
            log_i(__func__, "invalidate one: {}", id.inner());

            if (should_flush) {
                if (auto res = co_await handle_flush(id); not res) {
                    log_e(__func__, "failed to flush {}: {}", id.inner(), err_msg(res.error()));
                }
            }

            if (auto entry = m_table.extract(id); not entry.empty()) {
                if (entry.mapped().dirty and not should_flush) {
                    log_w(__func__, "[{}] is dirty but invalidated without flush!", id.inner());
                }
                for (auto page : entry.mapped().pages | sv::values) {
                    m_pages.release(page);
                }
            }

            co_return 0;
        }

        /**
         * @brief Invalidate all file entries.
         *
         * This function never fails.
         *
         * Remove and release all page entries in lookup table.
         *
         * see: `LruCache::invalidate_all()`
         */
        AExpect<usize> handle_invalidate_all()
        {
            for (auto& [id, entry] : m_table) {
                if (auto res = co_await handle_flush(id); not res) {
                    log_e(__func__, "failed to flush {}: {}", id.inner(), err_msg(res.error()));
                }
                entry.pages.clear();
            }

            std::ignore = co_await handle_invalidate_fds(true);

            m_pages.release_all();

            co_return 0;
        }

        /**
         * @brief Invalidate/remove all cached fds.
         *
         * This function invalidates all file descriptor even though there are still reader/writer. Since the
         * creation of these fds are lazy, this operation effectively refreshed the fds with new ones from the
         * device. This is useful if transport mode is changed.
         *
         * see: `LruCache::invalidate_fds()`
         */
        AExpect<usize> handle_invalidate_fds(bool close)
        {
            auto to_close = Vec<u64>{};

            for (auto& entry : m_table | sv::values) {
                if (entry.read_fd) {
                    to_close.emplace_back(*entry.read_fd);
                    entry.read_fd.reset();
                }
                if (entry.write_fd) {
                    to_close.emplace_back(*entry.write_fd);
                    entry.write_fd.reset();
                }
            }

            if (close) {
                for (auto fd : to_close) {
                    if (auto res = co_await m_connection.close(fd); not res) {
                        log_e(__func__, "failure on closing fd [{}]: {}", fd, err_msg(res.error()));
                    }
                }
            }

            co_return 0;
        }

        /**
         * @brief Remove unused fds.
         *
         * While the function responsibility is cleaning stale fds, it also cleans free file entries in the
         * lookup table. A free file entry is defined by the function `FileEntry::is_free()`.
         *
         * see: `LruCache::clean_stale_fds()`
         */
        AExpect<usize> handle_clean_stale_fds()
        {
            using namespace std::chrono_literals;

            auto finished_fds    = std::vector<u64>{};
            auto stale_to_remove = std::vector<u64>{};

            log_d(__func__, "start erasing stale fds [count={}]", m_stale_fds.size());

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
                } else {
                    log_d(
                        __func__,
                        "stale but has reader/writer? [reader={}|writer={}] [read_in={}|write_in={}]",
                        entry.reader,
                        entry.writer,
                        entry.read_inflight,
                        entry.write_inflight
                    );
                }
            }

            for (auto i : stale_to_remove | sv::reverse) {
                m_stale_fds.erase(m_stale_fds.begin() + static_cast<isize>(i));
            }
            stale_to_remove.clear();

            // >> yielding point
            for (auto fd : finished_fds) {
                if (auto res = co_await m_connection.close(fd); not res) {
                    log_w(__func__, "failure on closing fd [{}]: {}", fd, err_msg(res.error()));
                }
            }

            finished_fds.clear();

            log_d(__func__, "finish erasing stale fds [count={}]", stale_to_remove.size());

            for (auto it = m_table.begin(); it != m_table.end();) {
                if (const auto& [id, entry] = *it; entry.is_free()) {
                    log_d(__func__, "remove free entry for [{}] {:?}", id.inner(), entry.path);
                    sr::for_each(entry.pages | sv::values, [&](PageId id) { m_pages.release(id); });
                    it = m_table.erase(it);
                } else {
                    ++it;
                }
            }

            co_return 0;
        }

        /**
         * @brief Set a new page size.
         *
         * With this, create a new `PageStore`.
         *
         * see: `LruCache::set_page_size()`
         */
        AExpect<usize> handle_set_page_size(usize new_page_size)
        {
            if (m_pages.page_size() == new_page_size) {
                co_return 0;
            }

            std::ignore = co_await handle_invalidate_all();    // already wait busy all here

            m_pages = PageStore{ static_cast<u32>(new_page_size), m_pages.max_pages() };
            log_i(__func__, "page size changed to: {}", new_page_size);

            co_return 0;
        }

        /**
         * @brief Set new max number of pages.
         *
         * With this, create a new `PageStore`.
         *
         * see: `LruCache::set_max_pages()`
         */
        AExpect<usize> handle_set_max_pages(usize new_max_pages)
        {
            if (m_pages.max_pages() == new_max_pages) {
                co_return 0;
            }

            std::ignore = co_await handle_invalidate_all();    // already wait busy all here

            m_pages = PageStore{ m_pages.page_size(), static_cast<u32>(new_max_pages) };
            log_i(__func__, "max pages can be stored changed to: {}", new_max_pages);

            co_return 0;
        }

        /**
         * @brief Add new file entry on the lookup table if not exists already.
         *
         * @param id The unique identifier of the file.
         * @param path Path to the file on the device.
         *
         * @return A new file entry with empty pages.
         */
        FileEntry& new_lookup_file(Id id, path::Path path)
        {
            auto [it, _] = m_table.try_emplace(id, std::map<usize, PageId>{}, path.owned());
            return it->second;
        }

        /**
         * @brief Look up for file entry in the cache.
         *
         * @param id The unique identifier of the file.
         *
         * @return A lookup entry of the file or `nullptr` if not exists.
         */
        FileEntry* lookup_file(Id id)
        {
            if (auto entries = m_table.find(id); entries != m_table.end()) {
                return &entries->second;
            }
            return nullptr;
        }

        /**
         * @brief Read file at specified page index.
         *
         * @param entry File entry in the lookup table.
         * @param out Output buffer.
         * @param id File unique identifier.
         * @param index Page index of the operation.
         * @param first First index of the read operation.
         * @param last Last index of the read operation.
         * @param offset Offset of the read operation.
         *
         * The `first` and `last` paramaters are related to the `offset` and `out` buffer size.
         */
        AExpect<usize> read_at(
            FileEntry& entry,
            Span<char> out,
            Id         id,
            usize      index,
            usize      first,
            usize      last,
            off_t      offset
        )
        {
            log_t(__func__, "read: [id={}|idx={}]", id.inner(), index);

            auto rlock = scoped_increment(entry.read_inflight);
            auto ptr   = co_await lookup_page_for_read(entry, { id, index });

            if (not ptr) {
                co_return Unexpect{ ptr.error() };
            }

            const auto page_size = static_cast<usize>(m_pages.page_size());

            auto& page = **ptr;

            auto local_offset = 0uz;
            auto local_size   = page_size;

            if (index == first) {
                local_offset = static_cast<usize>(offset) % page_size;
                local_size   = local_size - local_offset;
            }

            if (index == last) {
                auto off    = static_cast<usize>(offset) % page_size;
                local_size  = (out.size() + off - 1) % page_size + 1;
                local_size -= local_offset;
            }

            auto out_off = 0uz;
            if (index >= first + 1) {
                out_off = (index - first) * page_size - static_cast<usize>(offset) % page_size;
            }

            auto out_span = Span{ out.data() + out_off, local_size };
            auto read     = page.read(out_span, local_offset);

            co_return read;
        }

        /**
         * @brief Loo kup for a page or evict an old one to be used for read operation.
         *
         * @param entry File entry information on the lookup table.
         * @param key Unique key identifying a file and page index to be used for read operation.
         *
         * This function will wait for "busy" status to be gone on the specified key, then register a "busy"
         * status for using the same key if the lookup missed (cache miss).
         */
        AExpect<Page*> lookup_page_for_read(FileEntry& entry, PageKey key)
        {
            // cache hit
            if (auto it = entry.pages.find(key.index); it != entry.pages.end()) {
                auto [page, _] = m_pages.get(it->second, true);
                co_return &page;
            }

            // cache miss
            auto fd = co_await entry.get_read_fd_or_open(m_connection);
            if (not fd) {
                co_return Unexpect{ fd.error() };
            }

            auto [page_id, page] = co_await acquire_or_evict(key);

            auto offset = static_cast<off_t>(key.index * m_pages.page_size());
            auto len    = co_await on_miss(*fd, page.buf(), offset);

            if (not len) {
                co_return Unexpect{ len.error() };
            }

            // since I manually written into page buffer, I need to set the size manually as well
            auto total = static_cast<usize>(offset) + *len;
            page.set_size(static_cast<u32>(total));

            entry.pages.emplace(key.index, page_id);

            co_return &page;
        }

        /**
         * @brief Read file at specified page index.
         *
         * @param entry File entry in the lookup table.
         * @param in Input buffer.
         * @param id File unique identifier.
         * @param index Page index of the operation.
         * @param first First index of the write operation.
         * @param last Last index of the write operation.
         * @param offset Offset of the write operation.
         *
         * The `first` and `last` paramaters are related to the `offset` and `out` buffer size. This function
         * will mark the page as dirty at written bytes location only.
         */
        AExpect<usize> write_at(
            FileEntry&       entry,
            Span<const char> in,
            Id               id,
            usize            index,
            usize            first,
            usize            last,
            off_t            offset
        )
        {
            log_t(__func__, "write: [id={}|idx={}]", id.inner(), index);

            auto ptr = co_await lookup_page_for_write(entry, { id, index });
            if (not ptr) {
                co_return Unexpect{ ptr.error() };
            }

            const auto page_size = static_cast<usize>(m_pages.page_size());

            auto& page = **ptr;

            auto local_offset = 0uz;
            auto local_size   = page_size;

            if (index == first) {
                local_offset = static_cast<usize>(offset) % page_size;
                local_size   = local_size - local_offset;
            }

            if (index == last) {
                auto off    = static_cast<usize>(offset) % page_size;
                local_size  = (in.size() + off - 1) % page_size + 1;
                local_size -= local_offset;
            }

            auto in_off = 0uz;
            if (index >= first + 1) {
                in_off = (index - first) * page_size - static_cast<usize>(offset) % page_size;
            }

            auto in_span = Span{ in.data() + in_off, local_size };
            auto written = page.write(in_span, local_offset);
            page.set_dirty(local_offset, local_offset + written);

            co_return written;
        }

        /**
         * @brief Look up for a page or evict an old one to be used for write operation.
         *
         * @param entry File entry information on the lookup table.
         * @param key Unique key identifying a file and page index to be used for write operation.
         */
        AExpect<Page*> lookup_page_for_write(FileEntry& entry, PageKey key)
        {
            // cache hit
            if (auto it = entry.pages.find(key.index); it != entry.pages.end()) {
                auto [page, _] = m_pages.get(it->second, true);
                co_return &page;
            }

            // cache miss
            auto [page_id, page] = co_await acquire_or_evict(key);
            entry.pages.emplace(key.index, page_id);

            co_return &page;
        }

        /**
         * @brief Flush specified page to the device.
         *
         * @param fd Real file descriptor of file on the device.
         * @param page The page to be flushed.
         * @param key The associated unique key of the page that will be flushed.
         *
         * As like `lookup_page_for_read()`, this function will wait for "busy" status to be gone and then
         * register its own "busy" status using the same key.
         */
        AExpect<void> flush_at(u64 fd, Page& page, PageKey key)
        {
            log_t(__func__, "flush: [id={}|idx={}]", key.id.inner(), key.index);

            // when a flush operation of this key was inflight and then there is another flush operation of
            // this key, after the wait_busy() above completes, the page won't be dirty anymore, the latter
            // operation must return immediately
            if (not page.is_dirty()) {
                co_return Expect<void>{};
            }

            if (page.is_fully_dirty()) {
                auto off = key.index * m_pages.page_size();
                auto res = co_await on_flush(fd, page.buf(), static_cast<off_t>(off));

                page.clear_dirty();
                co_return res.transform(sink_void);
            }

            auto buf = page.buf();

            for (auto [start, end] : page.iter_dirty()) {
                auto off  = key.index * m_pages.page_size() + start;
                auto span = buf.subspan(start, end - start);

                if (auto res = co_await on_flush(fd, span, static_cast<off_t>(off)); not res) {
                    co_return Unexpect{ res.error() };
                }
            }

            page.clear_dirty();
            co_return Expect<void>{};
        }

        /**
         * @brief Evict the specified page from the associated lookup table entry.
         *
         * @param page The associated page.
         * @param key The associated key of the page.
         *
         * This function flush the page if the specified page is dirty.
         */
        Await<void> evict(Page& page, PageKey key)
        {
            auto entry = lookup_file(key.id);
            if (not entry) {
                log_c(__func__, "evict [id={}|idx={}] requested but no entry", key.id.inner(), key.index);
                co_return;
            }

            if (page.is_dirty()) {
                log_i(__func__, "force push page [id={}|idx={}]", key.id.inner(), key.index);

                auto wlock = scoped_increment(entry->write_inflight);
                auto fd    = co_await entry->get_write_fd_or_open(m_connection);

                if (not fd) {
                    log_c(__func__, "evict [id={}|idx={}] can't open file", key.id.inner(), key.index);
                } else if (auto res = co_await flush_at(*fd, page, key); not res) {
                    log_c(__func__, "failed to push page [id={}|idx={}]", key.id.inner(), key.index);
                }
            }

            page.set_size(0);
            entry->pages.erase(key.index);
        }

        /**
         * @brief Read data from device.
         *
         * @param fd Real file descriptor of file on the device.
         * @param out Output buffer to be written with data from device.
         * @param offset Read offset on the file.
         *
         * @return Number of bytes written into the buffer.
         */
        AExpect<usize> on_miss(u64 fd, Span<u8> out, off_t offset)
        {
            auto span = Span{ reinterpret_cast<char*>(out.data()), out.size() };
            return m_connection.read(fd, span, offset);
        }

        /**
         * @brief Write data to device.
         *
         * @param fd Real file descriptor of file on the device.
         * @param in Input buffer to be written to the device.
         * @param offset Write offset on the file.
         *
         * @return Number of bytes written into the device.
         */
        AExpect<usize> on_flush(u64 fd, Span<const u8> in, off_t offset)
        {
            auto written = 0uz;

            while (written < in.size()) {
                auto in_sub = in.subspan(written);
                auto off    = offset + static_cast<off_t>(written);

                auto span = Span{ reinterpret_cast<const char*>(in_sub.data()), in_sub.size() };
                auto res  = co_await m_connection.write(fd, span, static_cast<off_t>(off));

                if (not res) {
                    co_return Unexpect{ res.error() };
                }
                written += *res;
            }

            co_return written;
        }

        /**
         * @brief Get a new page from the LRU or evict old ones.
         *
         * @param key A key of the new page.
         */
        Await<Pair<PageId, Page&>> acquire_or_evict(PageKey key)
        {
            auto [id, old_key] = m_pages.acquire(key);
            auto& page         = m_pages.get(id, false).page;

            if (old_key) {    // eviction
                co_await evict(page, *old_key);
            }

            co_return Pair<PageId, Page&>{ id, page };
        }

        Connection& m_connection;
        Channel     m_channel;

        PageStore   m_pages;    // page storage as well as LRU
        LookupTable m_table;    // lookup table for fast page access

        Vec<Tup<Id, FdKind>> m_stale_fds;
    };
}

// lru_cache.hpp impl LruCache
namespace madbfs
{
    LruCache::~LruCache()                              = default;
    LruCache::LruCache(LruCache&&) noexcept            = default;
    LruCache& LruCache::operator=(LruCache&&) noexcept = default;

    LruCache::LruCache(Uniq<Impl> impl)
        : m_impl{ std::move(impl) }
    {
    }

    Await<LruCache> LruCache::create(Connection& connection, usize page_size, usize max_pages)
    {
        auto exec = co_await async::current_executor();
        co_return LruCache{ std::make_unique<Impl>(exec, connection, page_size, max_pages) };
    }

    Await<void> LruCache::initialize()
    {
        return m_impl->initialize();
    }

    Await<void> LruCache::shutdown()
    {
        return m_impl->shutdown();
    }

    AExpect<void> LruCache::hint_open(Id id, path::Path path, OpenMode mode)
    {
        auto op = op::HintOpen{ .id = id, .path = path, .mode = mode };
        co_return (co_await m_impl->queue(op)).transform(sink_void);
    }

    AExpect<void> LruCache::hint_close(Id id, OpenMode mode)
    {
        auto op = op::HintClose{ .id = id, .mode = mode };
        co_return (co_await m_impl->queue(op)).transform(sink_void);
    }

    AExpect<usize> LruCache::read(Id id, Span<char> out, off_t offset)
    {
        auto op = op::Read{ .id = id, .out = out, .offset = offset };
        co_return co_await m_impl->queue(op);
    }

    AExpect<usize> LruCache::write(Id id, Span<const char> in, off_t offset)
    {
        auto op = op::Write{ .id = id, .in = in, .offset = offset };
        co_return co_await m_impl->queue(op);
    }

    AExpect<void> LruCache::flush(Id id)
    {
        auto op = op::Flush{ .id = id };
        co_return (co_await m_impl->queue(op)).transform(sink_void);
    }

    AExpect<void> LruCache::truncate(Id id, usize old_size, usize new_size)
    {
        auto op = op::Truncate{ .id = id, .old_size = old_size, .new_size = new_size };
        co_return (co_await m_impl->queue(op)).transform(sink_void);
    }

    Await<void> LruCache::rename(Id id, path::Path new_name)
    {
        auto op     = op::Rename{ .id = id, .new_name = new_name };
        std::ignore = co_await m_impl->queue(op);    // should've never fail
    }

    Await<void> LruCache::invalidate_one(Id id, bool should_flush)
    {
        auto op     = op::InvalidateOne{ .id = id, .should_flush = should_flush };
        std::ignore = co_await m_impl->queue(op);    // even if fail, invalidation should be not fallible
    }

    Await<void> LruCache::invalidate_all()
    {
        auto op     = op::InvalidateAll{};
        std::ignore = co_await m_impl->queue(op);    // even if fail, invalidation should be not fallible
    }

    Await<void> LruCache::clean_stale_fds()
    {
        auto op     = op::CleanStaleFds{};
        std::ignore = co_await m_impl->queue(op);    // even if fail, invalidation should be not fallible
    }

    Await<void> LruCache::invalidate_fds(bool close)
    {
        auto op     = op::InvalidateFds{ .close = close };
        std::ignore = co_await m_impl->queue(op);    // even if fail, invalidation should be not fallible
    }

    Await<void> LruCache::set_page_size(usize new_page_size)
    {
        auto op     = op::SetPageSize{ .new_page_size = new_page_size };
        std::ignore = co_await m_impl->queue(op);    // no failure except catastrophic (allocation failure)
    }

    Await<void> LruCache::set_max_pages(usize new_max_pages)
    {
        auto op     = op::SetMaxPages{ .new_max_pages = new_max_pages };
        std::ignore = co_await m_impl->queue(op);    // no failure except catastrophic (allocation failure)
    }

    usize LruCache::page_size() const
    {
        return m_impl->page_size();
    }

    usize LruCache::max_pages() const
    {
        return m_impl->max_pages();
    }

    usize LruCache::current_pages() const
    {
        return m_impl->current_pages();
    }
}
