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
        if (offset >= m_page_size) {
            log_w({ "{}: offset exceed page size [{} vs {}]" }, __func__, offset, m_page_size);
            return 0;
        }
        auto size = std::min(static_cast<u32>(offset + in.size()), m_page_size);
        std::copy_n(in.data(), in.size(), m_data.get() + offset);
        m_size = size;
        return in.size();
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

        auto work = [&](usize index) noexcept -> AExpect<usize> {
            log_d({ "read: [id={}|idx={}]" }, id.inner(), index);

            auto key = PageKey{ id, index };

            if (auto queued = m_queue.find(key); queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto entry = m_table.find(key);
            if (entry == m_table.end()) {
                auto promise = saf::promise<Errc>{ co_await async::this_coro::executor };
                auto future  = promise.get_future().share();
                m_queue.emplace(key, std::move(future));

                auto map_entry = m_path_map.find(id);
                if (map_entry == m_path_map.end()) {
                    m_path_map.emplace(id, PathEntry{ 1uz, path.into_buf() });
                } else {
                    ++map_entry->second.count;
                }

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
                auto [p, _] = m_table.emplace(key, m_lru.begin());
                entry       = p;

                promise.set_value(Errc{});
                m_queue.erase(key);

                if (m_table.size() > m_max_pages) {
                    co_await evict(m_table.size() - m_max_pages);
                }
            }

            const auto& [_, page] = *entry;

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

        auto work = [&](usize index) noexcept -> AExpect<usize> {
            log_d({ "write: [id={}|idx={}]" }, id.inner(), index);

            auto key = PageKey{ id, index };

            if (auto queued = m_queue.find(key); queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            auto map_entry = m_path_map.find(id);
            if (map_entry == m_path_map.end()) {
                m_path_map.emplace(id, PathEntry{ 1uz, path.into_buf() });
            } else {
                ++map_entry->second.count;
            }

            auto entry = m_table.find(key);
            if (entry == m_table.end()) {
                m_lru.emplace_front(key, std::make_unique<char[]>(m_page_size), 0, m_page_size);
                auto [p, _] = m_table.emplace(key, m_lru.begin());
                entry       = p;

                if (m_table.size() > m_max_pages) {
                    co_await evict(m_table.size() - m_max_pages);
                }
            }

            const auto& [_, page] = *entry;

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

    AExpect<void> Cache::flush(Id id, usize size)
    {
        auto num_pages = size / m_page_size + (size % m_page_size != 0);

        // for (auto index : sv::iota(0uz, num_pages)) {
        auto work = [&](usize index) noexcept -> AExpect<void> {
            log_d({ "flush: flush [id={}|idx={}]" }, id.inner(), index);

            auto key = PageKey{ id, index };

            if (auto queued = m_queue.find(key); queued != m_queue.end()) {
                auto fut = queued->second;
                co_await fut.async_wait();
                if (auto err = fut.get(); static_cast<bool>(err)) {
                    co_return Unexpect{ err };
                }
            }

            if (auto entry = m_table.find(key); entry != m_table.end()) {
                if (auto page = entry->second; page->is_dirty()) {
                    auto data = std::make_unique<char[]>(m_page_size);
                    auto read = page->read({ data.get(), m_page_size }, 0);
                    page->set_dirty(false);

                    auto span = Span{ data.get(), read };
                    auto res  = co_await on_flush(id, span, static_cast<off_t>(index * m_page_size));
                    if (not res.has_value()) {
                        co_return Unexpect{ res.error() };
                    }
                }
            } else {
                log_i({ "flush: page skipped [id={}|idx={}]" }, id.inner(), index);
            }

            co_return Expect<void>{};
        };

        // writing in parallel is not a good idea :P
        for (auto work : sv::iota(0uz, num_pages) | sv::transform(work)) {
            if (auto res = co_await std::move(work); not res) {
                co_return Unexpect{ res.error() };
            }
        }

        co_return Expect<void>{};
    }

    void Cache::invalidate()
    {
        m_table.clear();
        m_lru.clear();
        m_queue.clear();
        m_path_map.clear();

        log_i({ "{}: cache invalidated" }, __func__);
    }

    void Cache::set_page_size(usize new_page_size)
    {
        m_page_size = new_page_size;
        m_table.clear();
        m_lru.clear();
        m_queue.clear();
        m_path_map.clear();

        log_i({ "{}: page size changed to: {}" }, __func__, new_page_size);
    }

    void Cache::set_max_pages(usize new_max_pages)
    {
        m_max_pages = new_max_pages;
        m_table.clear();
        m_lru.clear();
        m_queue.clear();
        m_path_map.clear();

        log_i({ "{}: max pages can be stored changed to: {}" }, __func__, new_max_pages);
    }

    AExpect<usize> Cache::on_miss(Id id, Span<char> out, off_t offset)
    {
        auto found = m_path_map.find(id);
        assert(found != m_path_map.end());

        // NOTE: if m_path_map is updated, the path may point to freed memory, so copy is made
        auto path = found->second.path;
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] cache miss, read from device..." }, __func__, id.inner(), idx, offset);
        co_return co_await m_connection.read(path, out, offset);
    }

    AExpect<usize> Cache::on_flush(Id id, Span<const char> in, off_t offset)
    {
        auto found = m_path_map.find(id);
        assert(found != m_path_map.end());

        // NOTE: if m_path_map is updated, the path may point to freed memory, so copy is made
        auto path = found->second.path;
        auto idx  = static_cast<usize>(offset) / m_page_size;

        log_d({ "{}: [id={}|idx={}] flush, write to device..." }, __func__, id.inner(), idx, offset);
        co_return co_await m_connection.write(path, in, offset);
    }

    Await<void> Cache::evict(usize size)
    {
        while (size-- > 0 and not m_lru.empty()) {
            auto page = std::move(m_lru.back());
            m_lru.pop_back();
            m_table.erase(page.key());

            if (not page.is_dirty()) {
                continue;
            }

            auto [id, idx] = page.key();
            log_i({ "{}: force push page [id={}|idx={}]" }, __func__, id.inner(), idx);

            auto offset = static_cast<off_t>(idx * m_page_size);
            if (auto res = co_await on_flush(id, page.buf(), offset); not res) {
                log_c({ "{}: failed to force push page [id={}|idx={}" }, __func__, id.inner(), idx);
            }

            auto found = m_path_map.find(id);
            assert(found != m_path_map.end());
            if (--found->second.count == 0) {
                m_path_map.erase(found);
            }
        }
    }
}
