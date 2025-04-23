#include "adbfsm/path/path.hpp"

namespace
{
    std::generator<adbfsm::Str> iter_path_impl(adbfsm::Str path)
    {
        using namespace adbfsm;

        assert(not path.empty() and path.front() == '/');
        co_yield "/";

        auto index = 1uz;

        while (index < path.size()) {
            while (index < path.size() and path[index] == '/') {
                ++index;
            }
            if (index >= path.size()) {
                co_return;
            }

            auto it = sr::find(path | sv::drop(index), '/');
            if (it == path.end()) {
                auto res = path.substr(index);
                co_yield res;
                co_return;
            }

            auto pos = static_cast<std::size_t>(it - path.begin());
            auto res = path.substr(index, pos - index);

            index = pos + 1;
            co_yield res;
        }
    }
}

namespace adbfsm::path
{
    std::generator<Str> Path::iter() const
    {
        return iter_path_impl(fullpath());
    }

    std::generator<Str> Path::iter_parent() const
    {
        return iter_path_impl(parent());
    }

    Opt<Path> create(Str path)
    {
        if (path.empty() or path.front() != '/') {
            return std::nullopt;
        }

        while (path.size() > 2 and path[0] == '/' and path[1] == '/') {
            path.remove_prefix(1);
        }
        while (path.size() > 1 and path.back() == '/') {
            path.remove_suffix(1);
        }

        if (path == "/") {
            return Path{ "/", "/" };
        }

        auto prev    = 1uz;
        auto current = 1uz;

        while (current < path.size()) {
            while (current < path.size() and path[current] == '/') {
                ++current;
            }
            current = path.find('/', current);
            if (current == Str::npos) {
                break;
            }
            prev = current;
        }

        const auto dirname_end = prev;

        // in case the basename contains repeated '//' like in the case '/home/user/documents/////note.md'
        auto basename_start = prev;
        while (path[basename_start] == '/') {
            ++basename_start;
        }

        return Path{ path.substr(0, dirname_end), path.substr(basename_start) };
    }

    Opt<std::generator<Str>> iter_str(Str path)
    {
        if (path.empty() or path.front() != '/') {
            return std::nullopt;
        }
        return iter_path_impl(path);
    }
}
