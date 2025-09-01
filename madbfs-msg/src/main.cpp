#include <filesystem>
#include <madbfs-common/ipc.hpp>
#include <madbfs-common/log.hpp>

#include <boost/json.hpp>
#include <boost/program_options.hpp>

#include <iostream>
#include <regex>

namespace po = boost::program_options;
namespace sv = madbfs::aliases::sv;
namespace sr = madbfs::aliases::sr;

enum Mode
{
    List,
    Message,
};

struct Exit
{
    int ret;
};

struct Args
{
    Mode                     mode;
    std::string              search_path;
    std::vector<std::string> message;
};

std::variant<Exit, Args> parse_args(int argc, char** argv)
{
    const char* default_list_path = ::getenv("XDG_RUNTIME_DIR");
    if (default_list_path == nullptr) {
        default_list_path = "/tmp";
    }
    const char* default_serial = ::getenv("ANDROID_SERIAL");

    auto list_path = std::string{};
    auto serial    = std::string{};

    auto desc = po::options_description{ "options" };
    desc.add_options()                    //
        ("help,h", "print help")          //
        ("version,v", "print version")    //
        ("list,l",
         "list mounted devices with active IPC")    //
        ("list-dir,d",
         po::value<std::string>(&list_path)->default_value(default_list_path),
         "specify the search directory for '--list' option")    //
        ("serial,s",
         default_serial ? po::value<std::string>(&serial)->default_value(default_serial)
                        : po::value<std::string>(&serial),
         "the serial number of the mounted device (can be omitted if 'ANDROID_SERIAL' env is defined)")    //
        ("message",
         po::value<std::vector<std::string>>(),
         "message to be passed to madbfs (positional arguments will be considered as part of this option)");

    auto pos = po::positional_options_description{};
    pos.add("message", -1);

    auto print_help = [&](bool err) {
        auto out = err ? stderr : stdout;
        fmt::println(out, "madbfs-msg: send message to active madbfs instance over IPC socket");
        fmt::println(out, "usage: [options] [message]");
        std::cerr << '\n' << desc << '\n';
        return Exit{ 1 };
    };

    if (argc == 1) {
        return print_help(true);
    }

    auto vm = po::variables_map{};
    try {
        po::store(po::command_line_parser{ argc, argv }.options(desc).positional(pos).run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        fmt::println(stderr, "{}", e.what());
        return Exit{ 2 };
    }

    if (vm.count("help")) {
        return print_help(false);
    }

    if (vm.count("version")) {
        fmt::println(stdout, "{}", MADBFS_VERSION_STRING);
        return Exit{ 0 };
    }

    if (vm.count("list")) {
        return Args{ .mode = Mode::List, .search_path = list_path, .message = {} };
    }

    if (not vm.count("serial")) {
        fmt::println(
            stderr,
            "error: android device must be specified using '--serial' option or using 'ANDROID_SERIAL' env "
            "variable"
        );
        return Exit{ 2 };
    }

    if (not vm.count("message")) {
        fmt::println(stderr, "error: no message is specified");
        return Exit{ 1 };
    }

    return Args{
        .mode        = Mode::Message,
        .search_path = {},
        .message     = vm["message"].as<std::vector<std::string>>(),
    };
}

int perform_list(std::string_view search_path)
{
    namespace fs = std::filesystem;

    auto path = fs::path{ search_path };

    if (not fs::exists(path)) {
        fmt::println(stderr, "error: path '{}' does not exists", search_path);
        return 1;
    } else if (not fs::is_directory(path)) {
        fmt::println(stderr, "error: path '{}' is not a directory", search_path);
        return 1;
    }

    struct Socket
    {
        std::string serial;
        fs::path    path;
    };

    auto sockets        = std::vector<Socket>{};
    auto longest_serial = 0l;

    for (auto entry : fs::directory_iterator{ path }) {
        if (not entry.is_socket()) {
            continue;
        }

        auto name  = entry.path().filename().string();
        auto re    = std::regex{ R"regex(madbfs@(.*?)\.sock)regex" };
        auto match = std::smatch{};

        if (std::regex_match(name, match, re)) {
            sockets.emplace_back(match[1], entry.path());
            longest_serial = std::max(longest_serial, match[1].length());
        }
    }

    if (sockets.empty()) {
        fmt::println("no active sockets at the moment");
        return 0;
    }

    std::ranges::sort(sockets, std::less{}, &Socket::path);

    fmt::println("active sockets:");
    for (const auto& [serial, path] : sockets) {
        fmt::println("\t- {:<{}} -> {}", serial, longest_serial, path.c_str());
    }

    return 0;
}

int send_message(std::span<const std::string> message)
{
    fmt::println("message: {}", message);
    fmt::println("error: unimplemented :P");
    return 1;
}

int main(int argc, char** argv)
{
    auto may_args = parse_args(argc, argv);
    if (may_args.index() == 0) {
        return std::get<0>(may_args).ret;
    }

    switch (auto args = std::get<1>(may_args); args.mode) {
    case List: return perform_list(args.search_path);
    case Message: return send_message(args.message);
    default: return 1;
    }
}
