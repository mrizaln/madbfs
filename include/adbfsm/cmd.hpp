#pragma once

#include "adbfsm/log.hpp"

#include <subprocess.hpp>

namespace adbfsm::cmd
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
        log_d({ "exec: {}" }, cmd);

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
     * @brief Execute a command prefixed with `adb` and return the output (blocking).
     *
     * @param cmd The command to be ran on the adb shell.
     *
     * @return The output of the command.
     */
    [[nodiscard]] inline Out exec_adb(Cmd cmd)
    {
        log_d({ "exec_adb: {::?}" }, cmd);

        auto opt = Opt{
            .cout  = subprocess::PipeOption::pipe,
            .cerr  = subprocess::PipeOption::pipe,
            .cwd   = {},
            .check = false,
            .env   = {},
        };

        auto prefix = { "adb" };
        cmd.insert(cmd.begin(), prefix.begin(), prefix.end());

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
    [[nodiscard]] inline Out exec_adb_shell(Cmd cmd)
    {
        log_d({ "exec_adb_shell: {::?}" }, cmd);

        auto opt = Opt{
            .cout  = subprocess::PipeOption::pipe,
            .cerr  = subprocess::PipeOption::pipe,
            .cwd   = {},
            .check = false,
            .env   = {},
        };

        auto prefix = { "adb", "shell" };
        cmd.insert(cmd.begin(), prefix.begin(), prefix.end());

        return subprocess::run(cmd, opt);
    }
}
