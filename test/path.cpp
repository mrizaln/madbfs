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
    Str  input;
    Str  parent;
    Str  filename;
    bool is_dir;
};

struct TestIter
{
    Str              input;
    std::vector<Str> iterated;
};

std::ostream& operator<<(std::ostream& out, const TestConstruct& test)
{
    auto [input, parent, filename, _] = test;
    fmt::print(out, "input: [{}] | dirname: [{}] | basename: [{}]", input, parent, filename);
    return out;
}

constexpr auto constructible_testcases = std::array{
    TestConstruct{
        .input    = "/",
        .parent   = "/",
        .filename = "/",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "//",
        .parent   = "/",
        .filename = "/",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "//////",
        .parent   = "/",
        .filename = "/",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "//////////////////",
        .parent   = "/",
        .filename = "/",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home",
        .parent   = "/",
        .filename = "home",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "/home//",
        .parent   = "/",
        .filename = "home",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "////home////",
        .parent   = "/",
        .filename = "home",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home/user",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "///home/user",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "/home/user////",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home///user",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "/home///user//",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home//////user//",
        .parent   = "/home",
        .filename = "user",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "///////home/user/projects/cpp/adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp/adbfsm////",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home/////user/projects/cpp/adbfsm",
        .parent   = "/home/////user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "//home/user/////projects/cpp/adbfsm////",
        .parent   = "/home/user/////projects/cpp",
        .filename = "adbfsm",
        .is_dir   = true,
    },
    TestConstruct{
        .input    = "/home/user/projects/cpp//////adbfsm",
        .parent   = "/home/user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = false,
    },
    TestConstruct{
        .input    = "/home/user/projects/../projects/../../user/projects/cpp//////adbfsm",
        .parent   = "/home/user/projects/../projects/../../user/projects/cpp",
        .filename = "adbfsm",
        .is_dir   = false,
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
    using adbfsm::path::operator""_path;
    using adbfsm::path::PathBuf;

    "Path must be correctly constructed"_test = [](const TestConstruct& test) {
        auto path = create(test.input);

        expect(path.has_value()) << "can't construct: " << test.input;

        if (path.has_value()) {
            expect(test.parent == path->parent()) << test;
            expect(test.filename == path->filename()) << test;
            expect(test.is_dir == path->is_dir()) << test;
        }
    } | constructible_testcases;

    "Path must not be constructed if it is ill-formed"_test = [](Str test) {
        auto path = create(test);
        expect(not path.has_value());

        if (path.has_value()) {
            ut::log << fmt::format(
                "somehow has value? case: {} -- {} | {}", test, path->parent(), path->filename()
            );
        }
    } | non_constructible_testcases;

    "Path iter should be able to be iterated from root to basename"_test = [](const TestIter& test) {
        auto path_iter = create(test.input).value();
        auto iterated  = path_iter.iter() | sr::to<std::vector>();
        expect(that % test.iterated == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;

    "Path iter should be able to be iterator from root to dirname"_test = [](const TestIter& test) {
        auto path_iter = create(test.input).value();
        auto iterated  = path_iter.iter_parent() | sr::to<std::vector>();
        auto expected  = test.iterated;
        expected.size() > 1 ? expected.pop_back() : void();
        expect(that % expected == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;

    "Path can be constructed using literals"_test = [] {
        auto path = "/home/user/projects/cpp/adbfsm"_path;
        expect(path.parent() == "/home/user/projects/cpp");
        expect(path.filename() == "adbfsm");
        expect(path.fullpath() == "/home/user/projects/cpp/adbfsm");
        expect(not path.is_dir());

        path = "/////home//user/projects////cpp/adbfsm////"_path;
        expect(path.parent() == "/home//user/projects////cpp");
        expect(path.filename() == "adbfsm");
        expect(path.fullpath() == "/home//user/projects////cpp/adbfsm");
        expect(path.is_dir());

        // path = "C:/Users/user0/Documents/Work and School/D"_path;      // won't compile
    };

    "PathBuf must be correctly constructed"_test = [](const TestConstruct& test) {
        auto path = create(test.input);

        expect(path.has_value()) << "can't construct: " << test.input;

        if (path.has_value()) {
            auto path_buf = PathBuf{ *path };

            expect(test.parent == path_buf.as_path().parent()) << test;
            expect(test.filename == path_buf.as_path().filename()) << test;
            expect(test.is_dir == path_buf.as_path().is_dir()) << test;

            expect((void*)path->fullpath().data() != (void*)path_buf.as_path().fullpath().data())
                << "Address should be different: " << test;
        }
    } | constructible_testcases;
}
