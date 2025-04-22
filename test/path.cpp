#include "adbfsm/path/path.hpp"

#include <boost/ut.hpp>

#include <fmt/base.h>
#include <fmt/std.h>

#include <array>
#include <iostream>

namespace ut = boost::ut;
using namespace adbfsm::aliases;

struct TestCase
{
    Str input;
    Str parent;
    Str filename;
};

std::ostream& operator<<(std::ostream& out, const TestCase& test)
{
    auto [input, parent, filename] = test;
    fmt::print(out, "input: [{}] | dirname: [{}] | basename: [{}]", input, parent, filename);
    return out;
}

constexpr auto constructible_testcases = std::array{
    TestCase{
        .input    = "/",
        .parent   = "/",
        .filename = "/",
    },
    TestCase{
        .input    = "//",
        .parent   = "/",
        .filename = "/",
    },
    TestCase{
        .input    = "//////",
        .parent   = "/",
        .filename = "/",
    },
    TestCase{
        .input    = "//////////////////",
        .parent   = "/",
        .filename = "/",
    },
    TestCase{
        .input    = "/home",
        .parent   = "/",
        .filename = "home",
    },
    TestCase{
        .input    = "/home//",
        .parent   = "/",
        .filename = "home",
    },
    TestCase{
        .input    = "////home////",
        .parent   = "/",
        .filename = "home",
    },
    TestCase{
        .input    = "/home/user",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "///home/user",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "/home/user////",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "/home///user",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "/home///user//",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "/home//////user//",
        .parent   = "/home",
        .filename = "user",
    },
    TestCase{
        .input    = "/home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "///////home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "/home/user/projects/cpp/adbfsm////",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "/home/////user/projects/cpp/adbfsm",
        .parent   = "/home/////user/projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "//home/user/////projects/cpp/adbfsm////",
        .parent   = "/home/user/////projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "/home/user/projects/cpp//////adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestCase{
        .input    = "/home/user/projects/../projects/../../user/projects/cpp//////adbfsm",
        .parent   = "/home/user/projects/../projects/../../user/projects/cpp",
        .filename = "adbfsm",
    },
};

constexpr auto non_constructible_testcases = std::array{
    "",
    "root",
    "user/projects/cpp/adbfsm",
    "C:/user/projects/cpp/adbfsm",
    "C:\\user\\projects\\cpp\\adbfsm",
    "ftp://user/projects/cpp/adbfsm",
    "https://google.com",
};

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::fatal, ut::log;

    using adbfsm::path::create;

    "path must be correctly constructed"_test = [](TestCase test) {
        auto path = create(test.input);

        expect(path.has_value()) << "can't construct: " << test.input;

        if (path.has_value()) {
            expect(test.parent == path->parent()) << test;
            expect(test.filename == path->filename()) << test;
        }
    } | constructible_testcases;

    "path must not be constructed"_test = [](Str test) {
        auto path = create(test);
        expect(not path.has_value());

        if (path.has_value()) {
            ut::log << fmt::format(
                "somehow has value? case: {} -- {} | {}", test, path->parent(), path->filename()
            );
        }
    } | non_constructible_testcases;
}
