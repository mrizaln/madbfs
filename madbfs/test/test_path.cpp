#include "madbfs/path.hpp"

#include <boost/ut.hpp>

#include <fmt/base.h>
#include <fmt/std.h>

#include <array>
#include <iostream>

namespace ut = boost::ut;
using namespace madbfs::aliases;

struct TestConstruct
{
    Str input;
    Str upper_parent;
    Str upper_filename;
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
    auto [input, upper_parent, upper_filename, parent, filename] = test;
    fmt::print(
        out,
        "\ninput         : [{}]"
        "\nupper_parent  : [{}]"
        "\nupper_filename: [{}]"
        "\ndirname       : [{}]"
        "\nbasename      : [{}]",
        input,
        upper_parent,
        upper_filename,
        parent,
        filename
    );
    return out;
}

constexpr auto constructible_testcases = std::array{
    TestConstruct{
        .input          = "/",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "/",
    },
    TestConstruct{
        .input          = "//",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "/",
    },
    TestConstruct{
        .input          = "//////",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "/",
    },
    TestConstruct{
        .input          = "//////////////////",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "/",
    },
    TestConstruct{
        .input          = "/home",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "home",
    },
    TestConstruct{
        .input          = "/home//",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "home",
    },
    TestConstruct{
        .input          = "////home////",
        .upper_parent   = "/",
        .upper_filename = "/",
        .parent         = "/",
        .filename       = "home",
    },
    TestConstruct{
        .input          = "/home/user",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "///home/user",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "/home/user////",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "/home///user",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "/home///user//",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "/home//////user//",
        .upper_parent   = "/",
        .upper_filename = "home",
        .parent         = "/home",
        .filename       = "user",
    },
    TestConstruct{
        .input          = "/home/user/projects/cpp/madbfs",
        .upper_parent   = "/home/user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/projects/cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "///////home/user/projects/cpp/madbfs",
        .upper_parent   = "/home/user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/projects/cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "/home/user/projects/cpp/madbfs////",
        .upper_parent   = "/home/user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/projects/cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "/home/////user/projects/cpp/madbfs",
        .upper_parent   = "/home/////user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/////user/projects/cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "//home/user/////projects////cpp/madbfs////",
        .upper_parent   = "/home/user/////projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/////projects////cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "/home/user/projects/cpp//////madbfs",
        .upper_parent   = "/home/user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/projects/cpp",
        .filename       = "madbfs",
    },
    TestConstruct{
        .input          = "/home/user/projects/../projects/../../user/projects/cpp//////madbfs",
        .upper_parent   = "/home/user/projects/../projects/../../user/projects",
        .upper_filename = "cpp",
        .parent         = "/home/user/projects/../projects/../../user/projects/cpp",
        .filename       = "madbfs",
    },
};

constexpr auto non_constructible_testcases = std::array{
    "",
    "root",
    "user/projects/cpp/madbfs",
    "C:/user/projects/cpp/madbfs",
    "C:\\user\\projects\\cpp\\madbfs",
    "ftp://user/projects/cpp/madbfs",
    "https://google.com",
};

const inline auto iter_testcases = std::array{
    TestIter{
        .input    = "/",
        .iterated = { },
    },
    TestIter{
        .input    = "////",
        .iterated = { },
    },
    TestIter{
        .input    = "/home",
        .iterated = { "home" },
    },
    TestIter{
        .input    = "/home/",
        .iterated = { "home" },
    },
    TestIter{
        .input    = "/home///",
        .iterated = { "home" },
    },
    TestIter{
        .input    = "////home",
        .iterated = { "home" },
    },
    TestIter{
        .input    = "////home////",
        .iterated = { "home" },
    },
    TestIter{
        .input    = "/home/user",
        .iterated = { "home", "user" },
    },
    TestIter{
        .input    = "/home////user/",
        .iterated = { "home", "user" },
    },
    TestIter{
        .input    = "/home/user///",
        .iterated = { "home", "user" },
    },
    TestIter{
        .input    = "/home/user//projects",
        .iterated = { "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home/user/projects/",
        .iterated = { "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home////////user/projects//",
        .iterated = { "home", "user", "projects" },
    },
    TestIter{
        .input    = "/home/user/projects/cpp/madbfs",
        .iterated = { "home", "user", "projects", "cpp", "madbfs" },
    },
    TestIter{
        .input    = "///home/////user/projects/cpp/madbfs",
        .iterated = { "home", "user", "projects", "cpp", "madbfs" },
    },
    TestIter{
        .input    = "/home//user//////projects/////cpp/madbfs////////",
        .iterated = { "home", "user", "projects", "cpp", "madbfs" },
    },
    TestIter{
        .input    = "/home/./user/projects/../projects/../../user/././projects/cpp//////madbfs",
        .iterated = {
            "home", ".", "user", "projects", "..", "projects", "..", "..", "user",
            ".", ".", "projects", "cpp", "madbfs",
        },
    },
};

int main()
{
    using namespace ut::literals;
    using namespace ut::operators;
    using ut::expect, ut::log, ut::that, ut::throws;

    using madbfs::path::create;
    using madbfs::path::operator""_path;
    using madbfs::path::PathBuf;

    "Path must be correctly constructed"_test = [](const TestConstruct& test) {
        auto comp_path = create(test.input);

        expect(comp_path.has_value()) << "can't construct: " << test.input;
        if (not comp_path.has_value()) {
            return;
        }

        auto&& [comp, path] = *comp_path;

        expect(test.parent == path.parent()) << test;
        expect(test.filename == path.filename()) << test;

        auto parent = path.parent_path();

        expect(path.parent() == parent.str()) << test;
        expect(test.upper_parent == parent.parent()) << test;
        expect(test.upper_filename == parent.filename()) << test;

        "extend must construct a valid PathBuf if name does not contain '/'"_test = [&] {
            auto new_path = parent.extend_copy(path.filename());
            if (path.is_root()) {
                expect(not new_path.has_value()) << "path filename contains '/'";
                return;
            }

            // full path may be modified: repeating '/' is truncated e.g "/home///user -> "/home/user"
            auto full = fmt::format("{}{}{}", path.parent(), parent.is_root() ? "" : "/", path.filename());

            expect(new_path->str() == full) << test;
            expect(new_path->parent() == path.parent()) << test;
            expect(new_path->filename() == path.filename()) << test;
        };
    } | constructible_testcases;

    "Path must not be constructed if it is ill-formed"_test = [](Str test) {
        auto comp_path = create(test);
        expect(not comp_path.has_value());

        if (comp_path.has_value()) {
            auto&& [comp, path] = *comp_path;

            ut::log << fmt::format(
                "somehow has value? case: {} -- {} | {}", test, path.parent(), path.filename()
            );
        }
    } | non_constructible_testcases;

    "Path component should be able to be iterated from root to basename"_test = [](const TestIter& test) {
        auto [comp, path] = create(test.input).value();
        auto iterated     = path.iter() | sr::to<std::vector>();
        expect(that % test.iterated == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;

    "Path component should be able to be iterated from root to dirname"_test = [](const TestIter& test) {
        auto [comp, path] = create(test.input).value();
        auto iterated     = path.parent_path().iter() | sr::to<std::vector>();
        auto expected     = test.iterated;
        expected.size() > 0 ? expected.pop_back() : void();
        expect(that % expected == iterated) << fmt::format("On input: {:?}", test.input);
    } | iter_testcases;

    "PathBuf can be constructed using literals"_test = [] {
        auto path = "/home/user/projects/cpp/madbfs"_path;
        expect(path.parent() == "/home/user/projects/cpp");
        expect(path.filename() == "madbfs");
        expect(path.str() == "/home/user/projects/cpp/madbfs");

        path = "/////home//user/projects////cpp/madbfs////"_path;
        expect(path.parent() == "/home//user/projects////cpp");
        expect(path.filename() == "madbfs");
        expect(path.str() == "/home//user/projects////cpp/madbfs");

        expect(throws([] { "C:/Users/user0/Documents/Work and School/D"_path; }));
    };

    "PathBuf must be correctly constructed"_test = [](const TestConstruct& test) {
        auto comp_path = create(test.input);

        expect(comp_path.has_value()) << "can't construct: " << test.input;

        if (comp_path.has_value()) {
            auto&& [comp, path] = *comp_path;

            auto path_buf = path.owned();

            expect(test.parent == path_buf.parent()) << test;
            expect(test.filename == path_buf.filename()) << test;

            expect((void*)path.str().data() != (void*)path_buf.str().data())
                << "Address should be different: " << test;
        }
    } | constructible_testcases;
}
