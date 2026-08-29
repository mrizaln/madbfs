#pragma once

#include "madbfs/cache/lru_cache.hpp"
#include "madbfs/file_handle_store.hpp"
#include "madbfs/path.hpp"
#include "madbfs/tree/node.hpp"
#include "madbfs/tree/tree.hpp"

#include <madbfs-common/async/async.hpp>
#include <madbfs-common/util/var_wrapper.hpp>

#include <functional>

namespace madbfs
{
    class Connection;
}

namespace madbfs
{
    /**
     * @class Caching
     *
     * @brief Parameters to be passed into Cache constructor.
     */
    struct Caching
    {
        usize page_size;
        usize max_pages;
    };

    /**
     * @class Filesystem
     *
     * @brief A class representing the filesystem and its tree structure.
     *
     * This data structure is a Trie.
     */
    class Filesystem
    {
    public:
        using Filler = std::move_only_function<bool(const char*, const struct stat*, off_t)>;

        /**
         * @brief Create a new filesystem.
         *
         * @param ctx Async context.
         * @param connection Reference to active conneciton to device.
         * @param caching Cache parameters or empty for no caching.
         * @param ttl Filesystem node's stat expiration time before re-fetching.
         */
        Filesystem(async::Context& ctx, Connection& connection, Opt<Caching> caching, Opt<Seconds> ttl);

        /**
         * @brief Destroy filesystem.
         *
         * You must call `stop()` before destruction. Any asynchronous operation that is still happening may
         * not be stopped correctly otherwise.
         */
        ~Filesystem() = default;

        Filesystem(tree::Node&& root)            = delete;
        Filesystem& operator=(tree::Node&& root) = delete;

        Filesystem(const tree::Node& root)            = delete;
        Filesystem& operator=(const tree::Node& root) = delete;

        /**
         * @brief Get a node by the given path.
         *
         * @param path The path to the node.
         */
        Expect<Id> traverse(path::Path path);

        /**
         * @brief [TODO:description]
         *
         * @param path [TODO:parameter]
         */
        Expect<tree::Entry> traverse_entry(path::Path path);

        // fuse operations
        // ---------------
        AExpect<void> readdir(path::Path path, Filler filler, off_t offset);
        AExpect<void> getattr(path::Path path, struct stat* stbuf);

        AExpect<Str>  readlink(path::Path path);
        AExpect<Id>   mknod(path::Path path, mode_t mode, dev_t dev);
        AExpect<Id>   mkdir(path::Path path, mode_t mode);
        AExpect<void> unlink(path::Path path);
        AExpect<void> rmdir(path::Path path);
        AExpect<void> rename(path::Path from, path::Path to, u32 flags);
        AExpect<void> utimens(path::Path path, timespec atime, timespec mtime);
        AExpect<void> truncate(path::Path path, off_t size);

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
        // ---------------

        // This function only used to link already existing files, user can't and shouldn't use it
        Expect<void> symlink(path::Path path, Str target);

        /**
         * @brief Initialize the filesystem.
         *
         * This initializes the cache and fetch root directory stat.
         */
        AExpect<void> initialize(path::Path path);

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
        const Opt<cache::LruCache>& cache() const { return m_cache; }

        /**
         * @brief Get cache structure.
         */
        Opt<cache::LruCache>& cache() { return m_cache; }

        /**
         * @brief Get root node.
         */
        const tree::Tree& tree() const { return m_tree; }

        /**
         * @brief Get TTL.
         *
         * If expiration is not enabled it will return `std::nullopt`.
         */
        Opt<Seconds> ttl() const { return m_ttl; }

        /**
         * @brief Get open file handle store.
         */
        const FileHandleStore& handles() { return m_handles; }

    private:
        /**
         * @brief Fetch file stat from remote at `path` then create a child node on `parent`.
         *
         * @param parent_id The Id of the parent node.
         * @param path Path to the file.
         *
         * # Precondition:
         * - Node pointed by `parent_id` exists and is a directory.
         */
        AExpect<Id> build(Id parent_id, path::Path path);

        /**
         * @brief Same as build but force directory only, fails with `Errc::not_a_directory` if not directory.
         *
         * @param parent_id The Id of the parent node.
         * @param path Path to the file.
         *
         * # Precondition:
         * - Node pointed by `parent_id` exists and is a directory.
         */
        AExpect<Id> build_directory(Id parent_id, path::Path path);

        /**
         * @brief Traverse the node or build a new node.
         *
         * @param path Path to the node.
         */
        AExpect<Id> traverse_or_build(path::Path path);

        /**
         * @brief [TODO:description]
         *
         * @param path [TODO:parameter]
         */
        AExpect<tree::Entry> traverse_or_build_entry(path::Path path);

        /**
         * @brief Re-fetch file stat from remote and update the node accordingly.
         *
         * @param node The node in question.
         * @param id The Id of the node to be updated.
         * @param path Path to the corresponding file on remote.
         */
        AExpect<void> update(tree::Node& node, Id id, path::Path path);

        /**
         * @brief [TODO:description]
         *
         * @param left [TODO:parameter]
         * @param right [TODO:parameter]
         *
         * Preconditions:
         * - `left` and `right` nodes are not Error nodes.
         * - The parents of them must be verified of its existence and their types as directory as well.
         *
         * This function updates the stat of both `left` and `right` parents.
         */
        AExpect<void> exchange_nodes(tree::Entry left, tree::Entry right);

        /**
         * @brief Move `left` node to `new_parent` as `new_name`.
         *
         * @param left [TODO:parameter]
         * @param new_name [TODO:parameter]
         * @param new_parent [TODO:parameter]
         *
         * Preconditions:
         * - `left` is not error node.
         * - `new_parent` must be a directory.
         * - The parent of `left` must be verified of its existence and their types as directory as well.
         *
         * This will overwrite anything on `new_parent` that has the same name as `new_name`.
         * This function updates the stat of `new_parent` and `left`'s parent.
         */
        AExpect<void> move_node(tree::Entry left, Str new_name, tree::Entry new_parent);

        /**
         * @brief [TODO:description]
         *
         * @param dir [TODO:parameter]
         */
        AExpect<void> refresh_dir(Id id, path::Path path);

        /**
         * @brief Visit all nodes while doing operation on them.
         *
         * @param func The opeartion to be applied on each of the node.
         */
        void walk(Id start, std::function<void(tree::Entry)> func);

        /**
         * @brief Replace the node variant with the new one while invalidating the current one.
         *
         * @param node Node to be mutated.
         * @param id The Id of the node to be mutated.
         * @param file The new variant of the node.
         *
         * Invlidate here means removing the children (recursive) references from `Cache` as well as from
         * `FileHandleStore` if the node is a directory. The funciton will remove references of the node if
         * it is a regular file.
         */
        Await<void> mutate_and_invalidate(tree::Node& node, Id id, tree::File file);

        Connection& m_connection;

        tree::Tree           m_tree;
        Opt<cache::LruCache> m_cache;
        FileHandleStore      m_handles;

        Opt<Seconds> m_ttl = std::nullopt;
    };
}
