#include "madbfs/data/cache.hpp"

#include "madbfs/connection/connection.hpp"

#include <madbfs-common/log.hpp>

// NOTE: writing all this just for me to be able to write a short, nice, parallel_group spawner wrapper. what
// a joke. maybe I should use Rust after all. it has an actual type inference and it is very powerful, unlike
// C++ who almost has to nothing (auto and decltype is just too insignificant)
namespace
{
    template <typename Sig>
    struct AExpectFnTraits;

    // only works on non-mutable lambda with noexcept specifier
    template <typename C, typename R, typename... Args>
    struct AExpectFnTraits<madbfs::AExpect<R> (C::*)(Args...) const noexcept>
    {
        static constexpr auto void_ret = std::same_as<void, R>;

        using Ret = std::conditional_t<void_ret, madbfs::Unit, R>;

        template <template <typename...> typename Tmpl>
        using RetAsInner = madbfs::AExpect<std::conditional_t<void_ret, madbfs::Unit, Tmpl<Ret>>>;
    };

    // only works on non-mutable lambda with noexcept specifier
    template <typename Fn>
        requires std::is_class_v<std::decay_t<Fn>>
    struct AExpectFnTraits<Fn> : AExpectFnTraits<decltype(&std::decay_t<Fn>::operator())>
    {
    };

    template <typename Fn, template <typename...> typename Tmpl>
    using AExpectInnerFromFn = AExpectFnTraits<Fn>::template RetAsInner<Tmpl>;
}

namespace madbfs::data
{
    template <VRange R, Invocable<RangeValue<R>> Fn>
    AExpectInnerFromFn<Fn, Vec> spawn_parallel(Fn&& fn_coro, R&& args)
    {
        auto exec  = co_await asio::this_coro::executor;
        auto defer = [&]<typename Arg>(Arg&& arg) {
            return async::spawn(exec, std::forward<Fn>(fn_coro)(std::forward<Arg>(arg)), asio::deferred);
        };

        auto works = args | sv::transform(defer) | sr::to<std::vector>();
        auto grp   = asio::experimental::make_parallel_group(std::move(works));

        auto [ord, e, res] = co_await grp.async_wait(asio::experimental::wait_for_all{}, asio::use_awaitable);
        assert(sr::all_of(e, [](auto e) { return e == nullptr; }));

        for (auto i : ord) {
            if (not res[i]) {
                co_return Unexpect{ res[i].error() };
            }
        }

        if constexpr (AExpectFnTraits<Fn>::void_ret) {
            co_return Unit{};
        } else {
            co_return std::move(res)                                              //
                | sv::transform([](auto&& v) { return std::move(v).value(); })    //
                | sr::to<std::vector>();
        }
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
            log_c({ "{}: [BUG] offset exceed page size [{} vs {}]" }, __func__, offset, m_page_size);
            return 0;
        }

        auto end = static_cast<u32>(offset + in.size());
        if (end > m_page_size) [[unlikely]] {
            // NOTE: getting here is a bug in implementation
            log_c({ "{}: [BUG] offset + size exceed page size [{} vs {}]" }, __func__, end, m_page_size);
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
    Cache::Cache(connection::Connection& connection, usize page_size, usize max_pages)
        : m_connection{ connection }
        , m_page_size{ std::bit_ceil(page_size) }
        , m_max_pages{ max_pages }
    {
    }

    AExpect<usize> Cache::read(Id id, path::Path path, Span<char> out, off_t offset)
    {
        auto first = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + out.size() - 1) / m_page_size;

        log_d({ "{}: start [id={}|idx={} - {}]" }, __func__, id.inner(), first, last);

        auto& entry = lookup(id, path)->get();

        auto work = [&](usize index) noexcept -> AExpect<usize> {
            log_t({ "read: [id={}|idx={}]" }, id.inner(), index);

            auto key = PageKey{ id, index };

            if (auto queued = m_queue.find(key); queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto page_entry = entry.pages.find(index);
            if (page_entry == entry.pages.end()) {
                auto promise = saf::promise<Errc>{ co_await async::this_coro::executor };
                auto future  = promise.get_future().share();
                m_queue.emplace(key, std::move(future));

                auto data    = std::make_unique<char[]>(m_page_size);
                auto span    = Span{ data.get(), m_page_size };
                auto may_len = co_await on_miss(id, span, static_cast<off_t>(index * m_page_size));
                if (not may_len) {
                    promise.set_value(may_len.error());
                    m_queue.erase(key);
                    co_return Unexpect{ may_len.error() };
                } else if (not m_queue.contains(key)) {
                    promise.set_value(Errc::operation_canceled);
                    co_return Unexpect{ Errc::operation_canceled };
                }

                m_lru.emplace_front(key, std::move(data), *may_len, m_page_size);
                auto [p, _] = entry.pages.emplace(index, m_lru.begin());
                page_entry  = p;

                promise.set_value(Errc{});
                m_queue.erase(key);

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
        };

        auto res = co_await spawn_parallel(work, sv::iota(first, last + 1));
        if (not res) {
            log_e({ "{}: failed to read page of id={}" }, __func__, id.inner());
            co_return Unexpect{ res.error() };
        }

        co_return sr::fold_left(res.value(), 0uz, std::plus{});
    }

    AExpect<usize> Cache::write(Id id, path::Path path, Span<const char> in, off_t offset)
    {
        auto first = static_cast<usize>(offset) / m_page_size;
        auto last  = (static_cast<usize>(offset) + in.size() - 1) / m_page_size;

        log_d({ "{}: start [id={}|idx={} - {}]" }, __func__, id.inner(), first, last);

        auto& entry = lookup(id, path)->get();

        m_table[id].dirty = true;

        auto work = [&](usize index) noexcept -> AExpect<usize> {
            log_t({ "write: [id={}|idx={}]" }, id.inner(), index);

            auto key = PageKey{ id, index };

            if (auto queued = m_queue.find(key); queued != m_queue.end()) {
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
        };

        auto res = co_await spawn_parallel(work, sv::iota(first, last + 1));
        if (not res) {
            log_e({ "{}: failed to read page of id={}" }, __func__, id.inner());
            co_return Unexpect{ res.error() };
        }

        co_return sr::fold_left(res.value(), 0uz, std::plus{});
    }

    AExpect<void> Cache::flush(Id id)
    {
        auto may_entry = lookup(id, std::nullopt);
        if (not may_entry) {
            co_return Expect<void>{};
        }

        if (not std::exchange(m_table[id].dirty, false)) {
            co_return Expect<void>{};
        }

        log_d({ "flush: start [id={}|idx={}]" }, id.inner(), may_entry->get().pages | sv::keys);

        auto work = [&](Page& page) noexcept -> AExpect<void> {
            log_t({ "flush: [id={}|idx={}]" }, id.inner(), page.key().index);

            if (auto queued = m_queue.find(page.key()); queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            if (page.is_dirty()) {
                auto data = std::make_unique<char[]>(m_page_size);
                auto read = page.read({ data.get(), m_page_size }, 0);
                page.set_dirty(false);

                auto span = Span{ data.get(), read };
                auto res  = co_await on_flush(id, span, static_cast<off_t>(page.key().index * m_page_size));
                if (not res.has_value()) {
                    co_return Unexpect{ res.error() };
                }
            }

            co_return Expect<void>{};
        };

        // writing in parallel is not a good idea :P
        for (auto [_, page] : may_entry->get().pages) {
            if (auto res = co_await work(*page); not res) {
                co_return Unexpect{ res.error() };
            }
        }

        co_return Expect<void>{};
    }

    AExpect<void> Cache::truncate(Id id, usize old_size, usize new_size)
    {
        auto may_entry = lookup(id, std::nullopt);
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

            log_t({ "{}: [id={}|idx={}]" }, __func__, id.inner(), index);

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

    Await<void> Cache::shutdown()
    {
        for (auto id : m_table | sv::keys) {
            if (auto res = co_await flush(id); not res) {
                auto msg = std::make_error_code(res.error()).message();
                log_e({ "{}: failed to flush {}: {}" }, __func__, id.inner(), msg);
            }
        }

        co_await evict(m_lru.size());
        m_queue.clear();

        log_d({ "{}: m_table size: {}" }, __func__, m_table.size());
        for (auto [id, entry] : m_table) {
            auto path = entry.path.as_path().fullpath();
            log_t({ "{}:     {}: {} {}" }, __func__, id.inner(), entry.pages | sv::keys, path);
        }
    }

    Await<void> Cache::invalidate_one(Id id, bool should_flush)
    {
        auto entry = m_table.extract(id);
        if (entry.empty()) {
            co_return;
        }

        if (should_flush) {
            if (auto res = co_await flush(id); not res) {
                auto msg = std::make_error_code(res.error()).message();
                log_e({ "{}: failed to flush {}: {}" }, __func__, id.inner(), msg);
            }
        }

        for (auto [_, page] : entry.mapped().pages) {
            m_lru.erase(page);
        }
    }

    Await<void> Cache::invalidate_all()
    {
        co_await shutdown();
        log_i({ "{}: cache invalidated" }, __func__);
    }

    Await<void> Cache::set_page_size(usize new_page_size)
    {
        co_await shutdown();
        m_page_size = new_page_size;
        log_i({ "{}: page size changed to: {}" }, __func__, new_page_size);
    }

    Await<void> Cache::set_max_pages(usize new_max_pages)
    {
        co_await shutdown();
        m_max_pages = new_max_pages;
        log_i({ "{}: max pages can be stored changed to: {}" }, __func__, new_max_pages);
    }

    // NOTE: std::unordered_map guarantees reference of its element valid even if new value inserted
    Opt<Ref<Cache::LookupEntry>> Cache::lookup(Id id, Opt<path::Path> path)
    {
        auto entries = m_table.find(id);
        if (entries == m_table.end()) {
            if (path) {
                auto [p, _] = m_table.emplace(id, LookupEntry{ .pages = {}, .path = path->into_buf() });
                entries     = p;
            } else {
                return std::nullopt;
            }
        }
        return entries->second;
    }

    AExpect<usize> Cache::on_miss(Id id, Span<char> out, off_t offset)
    {
        auto found = m_table.find(id);
        assert(found != m_table.end());

        // NOTE: if m_path_map is updated, the path may point to freed memory, so copy is made
        auto path = found->second.path;
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] cache miss, read from device..." }, __func__, id.inner(), idx, offset);
        co_return co_await m_connection.read(path, out, offset);
    }

    AExpect<usize> Cache::on_flush(Id id, Span<const char> in, off_t offset)
    {
        auto found = m_table.find(id);
        assert(found != m_table.end());

        // NOTE: if m_path_map is updated, the path may point to freed memory, so copy is made
        auto path = found->second.path;
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] flush, write to device..." }, __func__, id.inner(), idx, offset);
        co_return co_await m_connection.write(path, in, offset);
    }

    Await<void> Cache::evict(usize size)
    {
        while (size-- > 0 and not m_lru.empty()) {
            auto page      = std::move(m_lru.back());
            auto [id, idx] = page.key();

            m_lru.pop_back();

            if (page.is_dirty()) {
                log_i({ "{}: force push page [id={}|idx={}]" }, __func__, id.inner(), idx);

                auto offset = static_cast<off_t>(idx * m_page_size);
                if (auto res = co_await on_flush(id, page.buf(), offset); not res) {
                    log_c({ "{}: failed to force push page [id={}|idx={}" }, __func__, id.inner(), idx);
                }
            }

            // this is done last since on_flush requires entry to still exists
            auto& entry = lookup(id, std::nullopt)->get();
            entry.pages.erase(idx);
            if (entry.pages.empty()) {
                m_table.erase(id);
            }
        }
    }
}
