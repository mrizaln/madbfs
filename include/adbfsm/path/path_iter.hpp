#pragma once

#include "adbfsm/path/path.hpp"

namespace adbfsm::path
{
    class PathIter
    {
    public:
        friend Opt<PathIter> iter_str(Str path);

        std::optional<Str> next() noexcept
        {
            if (m_index >= m_path.size()) {
                return std::nullopt;
            } else if (m_index == 0 and m_path.size() > 0 and m_path.front() == '/') {
                ++m_index;
                return "/";
            }

            while (m_index < m_path.size() and m_path[m_index] == '/') {
                ++m_index;
            }
            if (m_index >= m_path.size()) {
                return std::nullopt;
            }

            auto it = sr::find(m_path | sv::drop(m_index), '/');
            if (it == m_path.end()) {
                auto res = m_path.substr(m_index);
                m_index  = m_path.size();    // mark end of line
                return res;
            }

            auto pos = static_cast<std::size_t>(it - m_path.begin());
            auto res = m_path.substr(m_index, pos - m_index);

            m_index = pos + 1;

            return res;
        }

        void reset() noexcept { m_index = 0; }

    private:
        PathIter(Str path)
            : m_index{ 0 }
            , m_path{ path }
        {
            assert(path.size() > 0 and path.front() == '/');
        }

        usize m_index;
        Str   m_path;
    };

    inline Opt<PathIter> iter_str(Str path)
    {
        if (path.size() == 0 or path.front() != '/') {
            return std::nullopt;
        }
        return PathIter{ path };
    }

    inline Opt<PathIter> iter_path(const Path& path)
    {
        return iter_str(path.fullpath());
    }
}
