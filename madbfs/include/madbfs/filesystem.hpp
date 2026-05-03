#pragma once

#include "madbfs/file_handle_store.hpp"
#include "madbfs/node.hpp"
#include "madbfs/path.hpp"

#include <functional>

namespace madbfs
{
    class Connection;
}

namespace madbfs
{
    /**
     * @class Filesystem
     *
     * @brief A class representing the filesystem and its tree structure.
     *
     * This data structure is a Trie
     */
    class Filesystem
    {
    public:
        using Filler = std::move_only_function<void(const char* name)>;

        /**
         * @brief Create a new filesystem.
         *
         * @param connection Reference to active conneciton to device.
         * @param page_size Cache page size.
         * @param max_pages Maximum number of pages to be saved on the cache.
         * @param ttl Filesystem node's stat expiration time before re-fetching.
         */
        Filesystem(Connection& connection, usize page_size, usize max_pages, Opt<Seconds> ttl);

        /**
         * @brief Destroy filesystem.
         *
         * You must call `stop()` before destruction. Any asynchronous operation that is still happening may
         * not be stopped correctly otherwise.
         */
        ~Filesystem() = default;

        Filesystem(Node&& root)            = delete;
        Filesystem& operator=(Node&& root) = delete;

        Filesystem(const Node& root)            = delete;
        Filesystem& operator=(const Node& root) = delete;

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Ref<Node>> traverse(path::Path path);

        // fuse oprations
        // --------------
        AExpect<void>            readdir(path::Path path, Filler filler);
        AExpect<data::NamedStat> getattr(path::Path path);

        AExpect<Str>       readlink(path::Path path);
        AExpect<Ref<Node>> mknod(path::Path path, mode_t mode, dev_t dev);
        AExpect<Ref<Node>> mkdir(path::Path path, mode_t mode);
        AExpect<void>      unlink(path::Path path);
        AExpect<void>      rmdir(path::Path path);
        AExpect<void>      rename(path::Path from, path::Path to, u32 flags);
        AExpect<void>      utimens(path::Path path, timespec atime, timespec mtime);
        AExpect<void>      truncate(path::Path path, off_t size);

        AExpect<u64>   open(path::Path path, int flags);
        AExpect<usize> read(u64 fd, Span<char> out, off_t offset);
        AExpect<usize> write(u64 fd, Str in, off_t offset);
        AExpect<void>  flush(u64 fd);
        AExpect<void>  release(u64 fd);

        AExpect<usize> copy_file_range(
            path::Path in_path,
            u64        in_fd,
            off_t      in_off,
            path::Path path_out,
            u64        out_fd,
            off_t      offset_out,
            size_t     size
        );
        // --------------

        // this function only used to link already existing files, user can't and shouldn't use it
        Expect<void> symlink(path::Path path, Str target);

        /**
         * @brief Shut down the filesystem and stop every async operation.
         *
         * Call this before destructor. This is needed to do proper flushing for the `Cache`.
         */
        Await<void> shutdown();

        /**
         * @brief Set a new TTL for file system nodes.
         *
         * @param ttl New TTl value (set to `std::nullopt` to disable)
         *
         * @return Old TTL value.
         */
        Opt<Seconds> set_ttl(Opt<Seconds> ttl);

        /**
         * @brief Mark all nodes as expired.
         */
        usize expires_all();

        /**
         * @brief Get cache structure.
         */
        template <typename Self>
        auto&& cache(this Self&& self)
        {
            return std::forward_like<Self>(self.m_cache);
        }

        /**
         * @brief Get root node.
         */
        const Node& root() const { return m_root; }

        /**
         * @brief Get TTL.
         *
         * If expiration is not enabled it will return `std::nullopt`.
         */
        Opt<Seconds> ttl() const { return m_ttl; }

        FileHandleStore& handles() { return m_handles; }

    private:
        /**
         * @brief Fetch file stat from remote at `path` then create a child node on `parent`.
         *
         * @param parent Parent on which the child node will be created.
         * @param path Path to the file.
         */
        AExpect<Ref<Node>> build(Node& parent, path::Path path);

        /**
         * @brief Same as build but force directory only, fails with `Errc::not_a_directory` if not directory.
         *
         * @param parent Parent on which the child node will be created.
         * @param path Path to the file.
         */
        AExpect<Ref<Node>> build_directory(Node& parent, path::Path path);

        /**
         * @brief Traverse the node or build a new node.
         *
         * @param path Path to the node.
         */
        AExpect<Ref<Node>> traverse_or_build(path::Path path);

        /**
         * @brief Re-fetch file stat from remote and update the node accordingly.
         *
         * @param node The node in question.
         * @param path Path to the corresponding file on remote.
         */
        AExpect<void> update(Node& node, path::Path path);

        /**
         * @brief Visit all nodes while doing operation on them.
         *
         * @param func The opeartion to be applied on each of the node.
         */
        void walk(std::function<void(Node&)> func);

        /**
         * @brief Helper function to create context for node operations.
         *
         * @param path Path of the node operated on.
         */
        Node::Context make_context(const path::Path& path)
        {
            return {
                .connection = m_connection,
                .cache      = m_cache,
                .fd_counter = m_fd_counter,
                .path       = path,
            };
        }

        Connection& m_connection;

        Node            m_root;
        data::Cache     m_cache;
        FileHandleStore m_handles;

        Opt<Seconds>     m_ttl              = std::nullopt;
        std::atomic<u64> m_fd_counter       = 0;
        bool             m_root_initialized = false;
    };
}
