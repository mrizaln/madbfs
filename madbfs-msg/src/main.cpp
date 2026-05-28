#include <madbfs-common/async/async.hpp>
#include <madbfs-common/ipc.hpp>
#include <madbfs-common/log.hpp>

#include <boost/json.hpp>
#include <boost/program_options.hpp>
#include <fmt/base.h>

#include <filesystem>
#include <iostream>
#include <regex>

namespace po    = boost::program_options;
namespace json  = boost::json;
namespace async = madbfs::async;
namespace fs    = std::filesystem;
namespace ipc   = madbfs::ipc;

enum Mode
{
    List,
    Message,
};

enum Color
{
    Never,
    Always,
    Auto,
};

struct Exit
{
    int ret;
};

struct Args
{
    Mode                     mode;
    std::string              serial;
    std::string              search_path;
    std::vector<std::string> message;
    bool                     color;
};

struct Socket
{
    std::string serial;
    fs::path    path;
};

// forward decl
std::vector<Socket> get_socket_list(fs::path search_path);

std::istream& operator>>(std::istream& in, Color& c)
{
    auto string = std::string{};
    in >> string;

    // clang-format off
    if      (string == "never" ) { c = Color::Never;               }
    else if (string == "always") { c = Color::Always;              }
    else if (string == "auto"  ) { c = Color::Auto;                }
    else                         { in.setstate(std::ios::failbit); }
    // clang-format on

    return in;
}

std::variant<Exit, Args> parse_args(int argc, char** argv)
{
    const char* default_serial      = ::getenv("ANDROID_SERIAL");
    const char* default_search_path = ::getenv("XDG_RUNTIME_DIR");
    if (default_search_path == nullptr) {
        default_search_path = "/tmp";
    }

    auto search_path = std::string{};
    auto serial      = std::string{};
    auto color       = Color{};

    auto desc = po::options_description{ "options" };
    desc.add_options()                    //
        ("help,h", "print help")          //
        ("version,v", "print version")    //
        ("color,c",
         po::value<Color>(&color)->default_value(Color::Auto, "auto")->value_name("when"),
         "color the output (only for logcat) when=[never, always, auto] ")    //
        ("list,l",
         "list mounted devices with active IPC")    //
        ("search-dir,d",
         po::value<std::string>(&search_path)->default_value(default_search_path)->value_name("dir"),
         "specify the search directory for socket files")    //
        ("serial,s",
         default_serial ? po::value<std::string>(&serial)->default_value(default_serial)->value_name("serial")
                        : po::value<std::string>(&serial)->value_name("serial"),
         "the serial number of the mounted device (can be omitted if 'ANDROID_SERIAL' env is defined or only"
         " one device exists)")    //
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

    if (not fs::exists(search_path)) {
        fmt::println(stderr, "error: path '{}' does not exist", search_path.c_str());
        return Exit{ 1 };
    } else if (not fs::is_directory(search_path)) {
        fmt::println(stderr, "error: path '{}' is not a directory", search_path.c_str());
        return Exit{ 1 };
    }

    if (vm.count("list")) {
        return Args{
            .mode        = Mode::List,
            .serial      = {},
            .search_path = search_path,
            .message     = {},
            .color       = false,
        };
    }

    if (not vm.count("serial")) {
        auto sockets = get_socket_list(search_path);
        switch (sockets.size()) {
        case 0: {
            fmt::println(stderr, "error: no device found");
            return Exit{ 1 };
        } break;
        case 1: {
            serial = sockets.front().serial;
        } break;
        default: {
            fmt::println(stderr, "error: multiple device exists");
            for (auto&& [serial, _] : sockets) {
                fmt::println(stderr, "error:     - {}", serial);
            }
            fmt::println(stderr, "error: specify one in the command using '--serial' or 'ANDROID_SERIAL'");
            return Exit{ 1 };
        }
        }
    }

    if (not vm.count("message")) {
        fmt::println(stderr, "error: no message is specified");
        return Exit{ 1 };
    }

    auto should_color = false;
    switch (color) {
    case Always: should_color = true; break;
    case Auto: should_color = ::isatty(::fileno(stdout)) != 0; break;
    case Never: should_color = false; break;
    }

    return Args{
        .mode        = Mode::Message,
        .serial      = serial,
        .search_path = search_path,
        .message     = vm["message"].as<std::vector<std::string>>(),
        .color       = should_color,
    };
}

template <typename T, typename... Args>
    requires (std::default_initializable<Args> and ...)    //
         and std::constructible_from<T, Args...>           //
         and std::constructible_from<ipc::Op, T>
std::optional<ipc::Op> parse_cmd(std::string_view cmd, std::span<const std::string> args)
{
    using Tuple = std::tuple<Args...>;

    if (auto size = sizeof...(Args); args.size() != size) {
        fmt::println(stderr, "error: wrong number of argument on command '{}' (expects {} args)", cmd, size);
        return std::nullopt;
    }

    auto ok    = true;
    auto tuple = Tuple{};

    auto parse = [&]<std::size_t I>(std::string_view arg) {
        if (not ok) {
            return;
        } else if constexpr (std::integral<std::tuple_element_t<I, Tuple>>) {
            auto [ptr, ec] = std::from_chars(arg.begin(), arg.end(), std::get<I>(tuple));
            if (ptr != arg.end() or ec != std::errc{}) {
                fmt::println(stderr, "error: unable to parse '{}' to valid integer", arg);
                ok = false;
            }
        } else {
            std::get<I>(tuple) = arg;
        }
    };

    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (parse.template operator()<Is>(args[Is]), ...);
    }(std::index_sequence_for<Args...>{});

    return ok ? std::optional<ipc::Op>{ std::make_from_tuple<T>(std::move(tuple)) } : std::nullopt;
}

std::optional<ipc::Op> parse_message(std::span<const std::string> message)
{
    assert(not message.empty());

    auto cmd  = message[0];
    auto args = message.subspan(1);

    using Parser = std::optional<ipc::Op>(std::string_view cmd, std::span<const std::string> args);

    namespace op = ipc::op;

    static const auto parsers = std::unordered_map<std::string_view, Parser*>{ {
        // clang-format off
        { op::name::help,             parse_cmd<op::Help>                        },
        { op::name::version,          parse_cmd<op::Version>                     },
        { op::name::info,             parse_cmd<op::Info>                        },
        { op::name::invalidate_cache, parse_cmd<op::InvalidateCache>             },
        { op::name::expire_stat,      parse_cmd<op::ExpireStat>                  },
        { op::name::set_page_size,    parse_cmd<op::SetPageSize, unsigned long>  },
        { op::name::set_cache_size,   parse_cmd<op::SetCacheSize, unsigned long> },
        { op::name::set_ttl,          parse_cmd<op::SetTTL, unsigned long>       },
        { op::name::set_timeout,      parse_cmd<op::SetTimeout, unsigned long>   },
        { op::name::set_log_level,    parse_cmd<op::SetLogLevel, std::string>    },
        { op::name::logcat,           parse_cmd<op::Logcat>                      }, // let color unspecified
        { op::name::unmount,          parse_cmd<op::Unmount>                     },
        // clang-format on
    } };

    if (auto entry = parsers.find(cmd); entry != parsers.end()) {
        auto parser = entry->second;
        return parser(cmd, args);
    }

    fmt::println(stderr, "error: unknown command '{}'", cmd);
    return std::nullopt;
}

void pretty_print(json::value const& json, std::string* indent = nullptr)
{
    auto indent_str = std::string{};

    if (!indent) {
        indent = &indent_str;
    }

    switch (json.kind()) {
    case json::kind::object: {
        std::cout << "{\n";
        indent->append(4, ' ');
        auto const& obj = json.get_object();
        if (!obj.empty()) {
            for (auto it = obj.begin(); it != obj.end();) {
                std::cout << *indent << json::serialize(it->key()) << ": ";
                pretty_print(it->value(), indent);
                if (++it != obj.end()) {
                    std::cout << ",\n";
                }
            }
        }
        std::cout << "\n";
        indent->resize(indent->size() - 4);
        std::cout << *indent << "}";
    } break;
    case json::kind::array: {
        std::cout << "[\n";
        indent->append(4, ' ');
        auto const& arr = json.get_array();
        if (!arr.empty()) {
            for (auto it = arr.begin(); it != arr.end();) {
                std::cout << *indent;
                pretty_print(*it, indent);
                if (++it != arr.end()) {
                    std::cout << ",\n";
                }
            }
        }
        std::cout << "\n";
        indent->resize(indent->size() - 4);
        std::cout << *indent << "]";
    } break;
    case json::kind::string: std::cout << json::serialize(json.get_string()); break;
    case json::kind::uint64:
    case json::kind::int64:
    case json::kind::double_: std::cout << json; break;
    case json::kind::bool_: std::cout << (json.get_bool() ? "true" : "false"); break;
    case json::kind::null: std::cout << "null"; break;
    }

    if (indent->empty()) {
        std::cout << "\n";
    }
}

std::vector<Socket> get_socket_list(fs::path search_path)
{
    auto sockets = std::vector<Socket>{};

    for (auto entry : fs::directory_iterator{ search_path }) {
        if (not entry.is_socket()) {
            continue;
        }

        auto name  = entry.path().filename().string();
        auto re    = std::regex{ R"regex(madbfs@(.*?)\.sock)regex" };
        auto match = std::smatch{};

        if (std::regex_match(name, match, re)) {
            sockets.emplace_back(match[1], entry.path());
        }
    }

    return sockets;
}

int perform_list(fs::path search_path)
{
    auto sockets = get_socket_list(search_path);
    if (sockets.empty()) {
        fmt::println("no active sockets at the moment");
        return 0;
    }

    std::ranges::sort(sockets, std::less{}, &Socket::path);

    auto max_serial_len = 0uz;
    for (const auto& [serial, _] : sockets) {
        max_serial_len = std::max(max_serial_len, serial.size());
    }

    fmt::println("active sockets:");
    for (const auto& [serial, path] : sockets) {
        fmt::println("    - {:<{}} -> {}", serial, max_serial_len, path.c_str());
    }

    return 0;
}

int send_message(std::span<const std::string> message, fs::path socket_path, bool color)
{
    assert(not message.empty());

    auto op = parse_message(message);
    if (not op) {
        return 1;
    }

    auto context = async::Context{};
    auto client  = ipc::Client::create(context, socket_path.c_str());
    if (not client) {
        fmt::println(stderr, "error: failed to create client: {}", madbfs::err_msg(client.error()));
        return 1;
    }

    auto sig_set = madbfs::net::signal_set{ context, SIGINT, SIGTERM };
    sig_set.async_wait([&](auto, auto) { client->stop(); });

    auto coro = op->visit(madbfs::Overload{
        [&](this auto, ipc::FsOp op) -> madbfs::Await<int> {
            auto response = co_await client->send(op);
            if (not response) {
                fmt::println(stderr, "error: failed to send message: {}", madbfs::err_msg(response.error()));
                co_return 1;
            }

            pretty_print(*response);

            sig_set.cancel();
            co_return 0;
        },
        [&](this auto, ipc::op::Help) -> madbfs::Await<int> {
            auto response = co_await client->help();
            if (not response) {
                fmt::println(stderr, "error: failed to send message: {}", madbfs::err_msg(response.error()));
                co_return 1;
            }

            pretty_print(*response);

            sig_set.cancel();
            co_return 0;
        },
        [&](this auto, ipc::op::Version) -> madbfs::Await<int> {
            auto response = co_await client->version();
            if (not response) {
                fmt::println(stderr, "error: failed to send message: {}", madbfs::err_msg(response.error()));
                co_return 1;
            }

            pretty_print(*response);

            sig_set.cancel();
            co_return 0;
        },
        [&](this auto, ipc::op::Logcat) -> madbfs::Await<int> {
            auto response = co_await client->logcat({ .color = color });
            if (not response) {
                fmt::println(stderr, "error: failed to send message: {}", madbfs::err_msg(response.error()));
                co_return 1;
            }

            fmt::println("{:-^80}", "[ LOGCAT START ]");

            for (auto awaitable : *response) {
                auto message = co_await std::move(awaitable);
                if (not message) {
                    break;
                }

                fmt::println("{}", *message);
            }

            fmt::println("{:-^80}", "[ LOGCAT END ]");

            sig_set.cancel();
            co_return 0;
        },
    });

    return async::once(context, std::move(coro));
}

int main(int argc, char** argv)
try {
    auto parsed = parse_args(argc, argv);
    if (parsed.index() == 0) {
        return std::get<0>(parsed).ret;
    }

    auto args        = std::get<1>(parsed);
    auto search_path = fs::path{ args.search_path };

    switch (args.mode) {
    case List: return perform_list(search_path);
    case Message: {
        auto sockets = get_socket_list(search_path);
        auto socket  = std::ranges::find(sockets, args.serial, &Socket::serial);
        if (socket == sockets.end()) {
            fmt::println(stderr, "error: no socket for '{}' in '{}'", args.serial, search_path.c_str());
            return 1;
        }
        return send_message(args.message, socket->path, args.color);
    }
    default: return 1;
    }
} catch (const std::exception& e) {
    fmt::println(stderr, "error: exception occurred: {}", e.what());
    return 1;
} catch (...) {
    fmt::println(stderr, "error: exception occurred (unknown exception)");
    return 1;
}
