#pragma once

#include <madbfs-common/async/async.hpp>

namespace madbfs::cmd
{
    /**
     * @brief Execute a command.
     *
     * @param cmd Command to be run (args should be separated).
     * @param in Input data to be piped as stdin.
     * @param check Check return value.
     * @param merge_err Append stderr to stdout.
     *
     * @return Output of the command.
     */
    AExpect<String> exec(Span<const Str> cmd, Str in = "", bool check = true, bool merge_err = false);

    /**
     * @brief Execute a command.
     *
     * @param cmd Command to be run (args should be separated).
     * @param in Input data to be piped as stdin.
     * @param check Check return value.
     * @param merge_err Append stderr to stdout.
     *
     * @return Output of the command.
     *
     * NOTE: this overload should be removed when `std::initializer_list` support conversion into `std::span`.
     */
    inline AExpect<String> exec(Init<Str> cmd, Str in = "", bool check = true, bool merge_err = false)
    {
        auto span = Span<const Str>{ cmd.begin(), cmd.size() };
        co_return co_await exec(span, in, check, merge_err);
    }
}
