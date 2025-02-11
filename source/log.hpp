#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ranges.h>    // to enable ranges formatting
#include <spdlog/spdlog.h>

#include <source_location>

namespace adbfsm::log
{

    // inspired by this issue on spdlog: https://github.com/gabime/spdlog/issues/1959
    template <typename... Args>
    struct FmtWithLoc
    {
        fmt::format_string<Args...> m_fmt;
        spdlog::source_loc          m_loc;

        consteval static spdlog::source_loc to_spdlog_source_loc(const std::source_location& loc)
        {
            return { loc.file_name(), static_cast<int>(loc.line()), loc.function_name() };
        }

        template <typename Str>
        consteval FmtWithLoc(Str&& fmt, const std::source_location& loc = std::source_location::current())
            : m_fmt{ std::forward<Str>(fmt) }
            , m_loc{ to_spdlog_source_loc(loc) }
        {
        }

        FmtWithLoc(FmtWithLoc&& other)
            : m_fmt{ std::move(other.m_fmt) }
            , m_loc{ std::move(other.m_loc) }
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
     */
    inline void init(spdlog::level::level_enum level)
    {
        spdlog::set_default_logger(spdlog::stdout_color_mt("logger"));
        spdlog::set_pattern("[%Y-%m-%d|%H:%M:%S] [%^-%L-%$] [%s:%#] %v");
        spdlog::set_level(level);
    }

    /**
     * @brief Log a message with a specific log level.
     *
     * This functions must be called in the form of
     *
     * ```
     * adbfsm::log::log(spdlog::level::info, { "this is a test log: {} + {} = {}" }, 1, 2, 3);
       adbfsm::log::log(spdlog::level::info, { "akdjhaksdfjh" });
     * ```
     *
     * Notice the curly braces around the format string. If you don't use them, the compiler will complain.
     */
    template <typename... Args>
    inline void log(spdlog::level::level_enum level, FmtWithLoc<Args...> fmt, Args&&... args)
    {
        spdlog::log(fmt.m_loc, level, fmt.m_fmt, std::forward<Args>(args)...);
    }
}

namespace adbfsm
{
#define ADBFSM_LOG_LOG_ENTRY(Name, Level)                                                                    \
    template <typename... Args>                                                                              \
    inline void Name(log::FmtWithLoc<Args...> fmt, Args&&... args)                                           \
    {                                                                                                        \
        log::log(spdlog::level::Level, std::move(fmt), std::forward<Args>(args)...);                         \
    }

    ADBFSM_LOG_LOG_ENTRY(log_t, trace)
    ADBFSM_LOG_LOG_ENTRY(log_d, debug)
    ADBFSM_LOG_LOG_ENTRY(log_i, info)
    ADBFSM_LOG_LOG_ENTRY(log_w, warn)
    ADBFSM_LOG_LOG_ENTRY(log_e, err)
    ADBFSM_LOG_LOG_ENTRY(log_c, critical)

#undef ADBFSM_LOG_LOG_ENTRY
}
