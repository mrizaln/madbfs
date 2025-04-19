#include <boost/ut.hpp>

namespace ut = boost::ext::ut;

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::that;

    "hello"_test = [] {
        expect(that % 10 == 10);
        expect(that % 10 != 11);
    };
}
