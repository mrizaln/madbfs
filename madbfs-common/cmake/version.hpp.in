#pragma once

namespace madbfs::version
{
    /**
     * @brief Get the full version string
     *
     * If madbfs is built on a tagged commit the return value is the same as calling `get_version_base()`,
     * else the version is `{get_version_base()}-dev+g{get_git_hash()}`
     */

    const char* get_version_full();

    /**
     * @brief Get version string base.
     *
     * This will return the latest version tag from git.
     */
    const char* get_version_base();

    /**
     * @brief Get the git hash of when madbfs is built.
     */
    const char* get_git_hash();
}
