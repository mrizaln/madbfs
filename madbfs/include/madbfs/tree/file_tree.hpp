#pragma once

#include "madbfs/connection/connection.hpp"
#include "madbfs/path.hpp"
#include "madbfs/tree/node.hpp"

#include <functional>

namespace madbfs::tree
{
    /**
     * @class FileTree
     *
     * @brief A class representing a file tree structure.
     *
     * This data structure is a Trie
     */
    class FileTree
    {
    public:
        using Filler = std::move_only_function<void(const char* name)>;

        FileTree(connection::Connection& connection, data::Cache& cache, Opt<Seconds> ttl);
        ~FileTree() = default;

        FileTree(Node&& root)            = delete;
        FileTree& operator=(Node&& root) = delete;

        FileTree(const Node& root)            = delete;
        FileTree& operator=(const Node& root) = delete;

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Ref<Node>> traverse(path::Path path);

        // fuse oprations
        // --------------
        AExpect<void>                  readdir(path::Path path, Filler filler);
        AExpect<Ref<const data::Stat>> getattr(path::Path path);

        AExpect<Str>       readlink(path::Path path);
        AExpect<Ref<Node>> mknod(path::Path path, mode_t mode, dev_t dev);
        AExpect<Ref<Node>> mkdir(path::Path path, mode_t mode);
        AExpect<void>      unlink(path::Path path);
        AExpect<void>      rmdir(path::Path path);
        AExpect<void>      rename(path::Path from, path::Path to, u32 flags);

        AExpect<void>  truncate(path::Path path, off_t size);
        AExpect<u64>   open(path::Path path, int flags);
        AExpect<usize> read(path::Path path, u64 fd, Span<char> out, off_t offset);
        AExpect<usize> write(path::Path path, u64 fd, Str in, off_t offset);
        AExpect<void>  flush(path::Path path, u64 fd);
        AExpect<void>  release(path::Path path, u64 fd);
        AExpect<void>  utimens(path::Path path, timespec atime, timespec mtime);

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
         * @brief Get root node.
         */
        const Node& root() const { return m_root; }

        /**
         * @brief Get TTL.
         *
         * If expiration is not enabled it will return `std::nullopt`.
         */
        Opt<Seconds> ttl() const { return m_ttl; }

        /**
         * @brief Set a new TTL for file tree nodes.
         *
         * @param ttl New TTl value (set to `std::nullopt` to disable)
         *
         * @return Old TTL value.
         */
        Opt<Seconds> set_ttl(Opt<Seconds> ttl) { return std::exchange(m_ttl, ttl); }

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

        Node                    m_root;
        connection::Connection& m_connection;
        data::Cache&            m_cache;
        std::atomic<u64>        m_fd_counter       = 0;
        bool                    m_root_initialized = false;
        Opt<Seconds>            m_ttl              = std::nullopt;
    };
}
