#include "adbfsm/path/path_iter.hpp"

#include <boost/ut.hpp>
#include <fmt/format.h>
#include <opt_iter/opt_iter.hpp>

namespace ut = boost::ut;
using namespace adbfsm::aliases;

struct TestCase
{
    Str              input;
    std::vector<Str> iterated;
};

const inline auto constructible_testcases = std::array{
    TestCase{
        .input    = "/",
        .iterated = { "/" },
    },
    TestCase{
        .input    = "////",
        .iterated = { "/" },
    },
    TestCase{
        .input    = "/home",
        .iterated = { "/", "home" },
    },
    TestCase{
        .input    = "/home/",
        .iterated = { "/", "home" },
    },
    TestCase{
        .input    = "/home///",
        .iterated = { "/", "home" },
    },
    TestCase{
        .input    = "////home",
        .iterated = { "/", "home" },
    },
    TestCase{
        .input    = "////home////",
        .iterated = { "/", "home" },
    },
    TestCase{
        .input    = "/home/user",
        .iterated = { "/", "home", "user" },
    },
    TestCase{
        .input    = "/home////user/",
        .iterated = { "/", "home", "user" },
    },
    TestCase{
        .input    = "/home/user///",
        .iterated = { "/", "home", "user" },
    },
    TestCase{
        .input    = "/home/user//projects",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestCase{
        .input    = "/home/user/projects/",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestCase{
        .input    = "/home////////user/projects//",
        .iterated = { "/", "home", "user", "projects" },
    },
    TestCase{
        .input    = "/home/user/projects/cpp/adbfsm",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestCase{
        .input    = "///home/////user/projects/cpp/adbfsm",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestCase{
        .input    = "/home//user//////projects/////cpp/adbfsm////////",
        .iterated = { "/", "home", "user", "projects", "cpp", "adbfsm" },
    },
    TestCase{
        .input    = "/home/./user/projects/../projects/../../user/././projects/cpp//////adbfsm",
        .iterated = { 
            "/", "home", ".", "user", "projects", "..", "projects", "..", "..", "user",
            ".", ".", "projects", "cpp", "adbfsm",
        },
    },
};

const inline auto non_constructible_testcases = std::array{
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
    using ut::expect, ut::fatal, ut::log, ut::that;

    using adbfsm::path::iter_str;

    "path iter should be able to be iterated from root to basename"_test = [](const TestCase& test) {
        auto path_iter = iter_str(test.input);

        expect(path_iter.has_value()) << "can't construct: " << test.input;

        if (path_iter.has_value()) {
            auto storage  = std::optional<Str>{};
            auto iterated = opt_iter::make_with(storage, *path_iter) | sr::to<std::vector>();

            expect(that % test.iterated == iterated) << fmt::format("On input: {:?}", test.input);
        }
    } | constructible_testcases;

    "path iter should not be constructed"_test = [](Str test) {
        auto path_iter = iter_str(test);
        expect(not path_iter.has_value());

        if (path_iter.has_value()) {
            ut::log << fmt::format("somehow has value? on case: {}", test);
        }
    } | non_constructible_testcases;
}
