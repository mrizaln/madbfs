#pragma once

#include "madbfs-common/aliases.hpp"

#include <spdlog/fmt/ranges.h>    // to enable ranges formatting
#include <spdlog/fmt/std.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <source_location>

namespace madbfs::log
{
    using Level = spdlog::level::level_enum;

    constexpr static spdlog::source_loc to_spdlog_source_loc(const std::source_location& loc) noexcept
    {
        return { loc.file_name(), static_cast<int>(loc.line()), loc.function_name() };
    }

    // inspired by this issue on spdlog: https://github.com/gabime/spdlog/issues/1959
    template <typename... Args>
    struct FmtWithLoc
    {
        fmt::format_string<Args...> fmt;
        spdlog::source_loc          loc;

        template <typename Str>
        consteval FmtWithLoc(Str&& fmt, const std::source_location& loc = std::source_location::current())
            : fmt{ std::forward<Str>(fmt) }
            , loc{ to_spdlog_source_loc(loc) }
        {
        }

        FmtWithLoc(FmtWithLoc&& other)
            : fmt{ std::move(other.fmt) }
            , loc{ std::move(other.loc) }
        {
        }

        FmtWithLoc(const FmtWithLoc& other)            = delete;
        FmtWithLoc& operator=(const FmtWithLoc& other) = delete;
        FmtWithLoc& operator=(FmtWithLoc&& other)      = delete;
    };

    /**
     * @brief Initialize the logger at a specific log level with predefined pattern and sink.
     *
     * @param level The log level to use.
     * @param log_file The log file to write to. If set to "-", the logger will write to stdout.
     */
    inline void init(spdlog::level::level_enum level, Str log_file) noexcept
    {
        constexpr auto max_size  = 10 * 1000 * 1000_usize;    // 10 MB
        constexpr auto max_files = 3_usize;

        auto logger = [&] {
            if (log_file == "-") {
                return spdlog::stdout_color_mt("logger-stdout", spdlog::color_mode::always);
            } else {
                return spdlog::rotating_logger_mt("logger-file", log_file.data(), max_size, max_files);
            }
        }();

        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d|%H:%M:%S] [%^-%L-%$] [%s:%#] %v");
        spdlog::set_level(level);
    }

    /**
     * @brief Shut down logger.
     *
     * This function explicitly flushes the logger.
     */
    inline void shutdown() noexcept
    {
        if (auto logger = spdlog::get("logger-file"); logger) {
            logger->flush();
        }
        spdlog::shutdown();
    }

    inline Level get_level() noexcept
    {
        return spdlog::get_level();
    }

    /**
     * @brief Log a message with a specific log level.
     *
     * NOTE: The type identity is needed to allow CTAD for FmtWithLoc:
     * https://stackoverflow.com/a/79155521/16506263
     */
    template <typename... Args>
    inline void log(
        spdlog::level::level_enum                 level,
        FmtWithLoc<std::type_identity_t<Args>...> fmt,
        Args&&... args
    )
    {
        spdlog::log(fmt.loc, level, fmt.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void log_loc(
        std::source_location        loc,
        spdlog::level::level_enum   level,
        fmt::format_string<Args...> fmt,
        Args&&... args
    )
    {
        spdlog::log(to_spdlog_source_loc(loc), level, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    inline void log_exception(
        std::exception_ptr   e,
        Str                  prefix,
        std::source_location loc = std::source_location::current()
    )
    {
        try {
            e ? std::rethrow_exception(e) : void();
        } catch (const std::exception& e) {
            log_loc(loc, Level::critical, "{}: exception occurred: {}", prefix, e.what());
        } catch (...) {
            log_loc(loc, Level::critical, "{}: exception occurred (unknown exception)", prefix);
        }
    }
}

namespace madbfs
{
#define MADBFS_LOG_LOG_ENTRY(Name, Level)                                                                    \
    template <typename... Args>                                                                              \
    inline void Name(log::FmtWithLoc<std::type_identity_t<Args>...> fmt, Args&&... args)                     \
    {                                                                                                        \
        log::log(spdlog::level::Level, std::move(fmt), std::forward<Args>(args)...);                         \
    }

    MADBFS_LOG_LOG_ENTRY(log_t, trace)
    MADBFS_LOG_LOG_ENTRY(log_d, debug)
    MADBFS_LOG_LOG_ENTRY(log_i, info)
    MADBFS_LOG_LOG_ENTRY(log_w, warn)
    MADBFS_LOG_LOG_ENTRY(log_e, err)
    MADBFS_LOG_LOG_ENTRY(log_c, critical)

#undef MADBFS_LOG_LOG_ENTRY
}
