#pragma once

#include "madbfs/cache/lru_cache.hpp"
#include "madbfs/file_handle_store.hpp"
#include "madbfs/orphan_store.hpp"
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
         * @param root_path Path to the subdirectory on device that will be considered as the root of the
         * filesystem.
         */
        static AExpect<Filesystem> create(
            Connection&      connection,
            cache::LruCache* cache,
            path::Path       root_path
        );

        /**
         * @brief Destroy filesystem.
         *
         * You must call `stop()` before destruction. Any asynchronous operation that is still happening may
         * not be stopped correctly otherwise.
         */
        ~Filesystem() = default;

        Filesystem(Filesystem&& root)                     = default;
        Filesystem& operator=(Filesystem&& root) noexcept = delete;

        Filesystem(const tree::Node& root)            = delete;
        Filesystem& operator=(const tree::Node& root) = delete;

        // fuse low-level operations
        // -------------------------
        AExpect<void> ll_lookup(Id parent, Str name, struct stat* stbuf);
        AExpect<void> ll_forget(Id id, u64 nlookup);
        AExpect<void> ll_getattr(Id id, struct stat* stbuf);
        AExpect<void> ll_setattr(
            Id            id,
            Opt<u64>      size,
            Opt<timespec> atime,
            Opt<timespec> mtime,
            struct stat*  stbuf
        );
        AExpect<Str>   ll_readlink(Id id);
        AExpect<void>  ll_mknod(Id parent, Str name, mode_t mode, dev_t dev, struct stat* stbuf);
        AExpect<void>  ll_mkdir(Id parent, Str name, mode_t mode, struct stat* stbuf);
        AExpect<void>  ll_unlink(Id parent, Str name);
        AExpect<void>  ll_rmdir(Id parent, Str name);
        AExpect<void>  ll_rename(Id parent, Str name, Id new_parent, Str new_name, u32 flags);
        AExpect<u64>   ll_open(Id id, i32 flags);
        AExpect<usize> ll_read(u64 fd, Span<char> out, off_t offset);
        AExpect<usize> ll_write(u64 fd, Span<const char> in, off_t offset);
        AExpect<void>  ll_release(u64 fd);
        AExpect<void>  ll_fsync(u64 fd, bool data_only);
        AExpect<void>  ll_readdir(Id id, off_t offset, Filler filler);
        AExpect<usize> ll_copy_file_range(
            Id     in_id,
            u64    in_fd,
            off_t  in_off,
            Id     out_id,
            u64    out_fd,
            off_t  out_off,
            size_t size
        );
        // -------------------------

        /**
         * @brief Visit all nodes while doing operation on them.
         *
         * @param func The opeartion to be applied on each of the node.
         */
        usize walk(Id start, std::function<void(tree::Entry)> func);

        /**
         * @brief Get cache structure.
         */
        const cache::LruCache* cache() const { return m_cache; }

        /**
         * @brief Get cache structure.
         */
        cache::LruCache* cache() { return m_cache; }

        /**
         * @brief Get filesystem tree.
         */
        const tree::Tree& tree() const { return m_tree; }

        /**
         * @brief Get open file handle store.
         */
        const FileHandleStore& handles() { return m_handles; }

    private:
        Filesystem(Connection& connection, cache::LruCache* cache, path::Path root_path, Stat root_stat);

        void fill_stbuf(const tree::Node& node, Id id, struct stat* stbuf);

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

        tree::Tree       m_tree;
        cache::LruCache* m_cache;    // may be null, Opt<T&> doesn't exist, curses to the C++ committeeee!
        FileHandleStore  m_handles;
        OrphanStore      m_orphans;
    };
}
