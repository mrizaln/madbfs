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
}
