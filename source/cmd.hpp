#pragma once

#include "common.hpp"
#include "log.hpp"

#include <subprocess.hpp>

#include <array>

namespace adbfs::cmd
{
    using Out = subprocess::CompletedProcess;
    using Cmd = subprocess::CommandLine;
    using Opt = subprocess::RunOptions;

    /**
     * @brief Execute a command and return the output (blocking).
     *
     * @param cmd The command to be ran.
     *
     * @return The output of the command.
     */
    [[nodiscard]] inline Out exec(Cmd cmd)
    {
        log_d({ "exec_command: {}" }, cmd);

        auto opt = Opt{
            .cout  = subprocess::PipeOption::pipe,
            .cerr  = subprocess::PipeOption::pipe,
            .cwd   = {},
            .check = false,
            .env   = {},
        };

        return subprocess::run(cmd, opt);
    }

    /**
     * @brief Execute a command on the adb shell and return the output (blocking).
     *
     * @param cmd The command to be ran on the adb shell.
     * @param serial The serial number of the device to run the command on.
     *
     * @return The output of the command.
     */
    [[nodiscard]] inline Out exec_adb(Cmd cmd, Str serial)
    {
        log_d({ "adb_shell [{}]: {}" }, serial, cmd);

        auto opt = Opt{
            .cout  = subprocess::PipeOption::pipe,
            .cerr  = subprocess::PipeOption::pipe,
            .cwd   = {},
            .check = false,
            .env   = { { std::string{ "ANDROID_SERIAL" }, std::string{ serial } } },
        };

        auto prefix = std::array{ "adb", "shell" };
        cmd.insert(cmd.begin(), prefix.begin(), prefix.end());

        return subprocess::run(cmd, opt);
    }
}
