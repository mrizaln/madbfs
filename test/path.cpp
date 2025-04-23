#include "adbfsm/path/path.hpp"

#include <boost/ut.hpp>

#include <fmt/base.h>
#include <fmt/std.h>

#include <array>
#include <iostream>

namespace ut = boost::ut;
using namespace adbfsm::aliases;

struct TestConstruct
{
    Str input;
    Str parent;
    Str filename;
};

struct TestIter
{
    Str              input;
    std::vector<Str> iterated;
};

std::ostream& operator<<(std::ostream& out, const TestConstruct& test)
{
    auto [input, parent, filename] = test;
    fmt::print(out, "input: [{}] | dirname: [{}] | basename: [{}]", input, parent, filename);
    return out;
}

constexpr auto constructible_testcases = std::array{
    TestConstruct{
        .input    = "/",
        .parent   = "/",
        .filename = "/",
    },
    TestConstruct{
        .input    = "//",
        .parent   = "/",
        .filename = "/",
    },
    TestConstruct{
        .input    = "//////",
        .parent   = "/",
        .filename = "/",
    },
    TestConstruct{
        .input    = "//////////////////",
        .parent   = "/",
        .filename = "/",
    },
    TestConstruct{
        .input    = "/home",
        .parent   = "/",
        .filename = "home",
    },
    TestConstruct{
        .input    = "/home//",
        .parent   = "/",
        .filename = "home",
    },
    TestConstruct{
        .input    = "////home////",
        .parent   = "/",
        .filename = "home",
    },
    TestConstruct{
        .input    = "/home/user",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "///home/user",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "/home/user////",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "/home///user",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "/home///user//",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "/home//////user//",
        .parent   = "/home",
        .filename = "user",
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
        .input    = "///////home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp/adbfsm////",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
        .input    = "/home/////user/projects/cpp/adbfsm",
        .parent   = "/home/////user/projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
        .input    = "//home/user/////projects/cpp/adbfsm////",
        .parent   = "/home/user/////projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp//////adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
    },
    TestConstruct{
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

const inline auto iter_testcases = std::array{
    TestIter{
        .input    = "/",
        .iterated = { "/" },
    },
    TestIter{
        .input    = "////",
        .iterated = { "/" },
    },
    TestIter{
        .input    = "/home",
        .iterated = { "/", "home" },
    },
    TestIter{
        .input    = "/home/",
        .iterated = { "/", "home" },
    },
    TestIter{
        .input    = "/home///",
        .iterated = { "/", "home" },
    },
    TestIter{
        .input    = "////home",
        .iterated = { "/", "home" },
    },
    TestIter{
        .input    = "////home////",
        .iterated = { "/", "home" },
    },
    TestIter{
        .input    = "/home/user",
        .iterated = { "/", "home", "user" },
    },
    TestIter{
        .input    = "/home////user/",
        .iterated = { "/", "home", "user" },
    },
    TestIter{
        .input    = "/home/user///",
        .iterated = { "/", "home", "user" },
    },
    TestIter{
        .input    = "/home/user//projects",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home/user/projects/",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home////////user/projects//",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home/user/projects/cpp/adbfsm",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestIter{
        .input    = "///home/////user/projects/cpp/adbfsm",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestIter{
        .input    = "/home//user//////projects/////cpp/adbfsm////////",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestIter{
        .input    = "/home/./user/projects/../projects/../../user/././projects/cpp//////adbfsm",
        .iterated = { 
            "/", "home", ".", "user", "projects", "..", "projects", "..", "..", "user",
            ".", ".", "projects", "cpp", "adbfsm",
        },
    },
};

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::fatal, ut::log, ut::that;

    using adbfsm::path::create;

    "path must be correctly constructed"_test = [](const TestConstruct& test) {
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

    "path iter should be able to be iterated from root to basename"_test = [](const TestIter& test) {
        auto path_iter = create(test.input).value();
        auto iterated  = path_iter.iter() | sr::to<std::vector>();
        expect(that % test.iterated == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;

    "path iter should be able to be iterator from root to dirname"_test = [](const TestIter& test) {
        auto path_iter = create(test.input).value();
        auto iterated  = path_iter.iter_parent() | sr::to<std::vector>();
        auto expected  = test.iterated;
        expected.size() > 1 ? expected.pop_back() : void();
        expect(that % expected == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;
}
