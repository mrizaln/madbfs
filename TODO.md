# TODO

- Server: i am thinking of maintaning a map of file descriptor with thread index. a file read/write operation will only be performed on associated thread. there would be then a distibution logic for thread allocation on each file.
- Store device file's `inode` on `Stat` for file change detection.
  - This detection method won't detect writes from outside `madbfs` but it will be more accurate compared to current behavior, comparing `mtime` cached vs `mtime` device, which might be desynchronized (even though I have set 2 seconds tolerance, the device will have different `mtime` vs from the cache because of the caching! and that might not be enough)
  - Maybe add file size detection as well since this one is easier to track (maybe add separate variable to store how many bytes has been written to cache but not flushed yet so that when the TTL triggers `update()` the compared sizes will be the same)
  - For directory `mtime` detection should be easier to implement
- Currently copying multiple files is incredibly slow. This is probably because on flush we wait for the result (MAYBE)
- Make LRU list elements `std::shared_ptr` since it is currently very flaky can crash when multiple files is copied at the same time
- Add test case for copying multiple files amounting to greater than the cache
- Add test case for copying file larger than the cache size (in 1MB chunks)
- Cache: review eviction logic, especially the force push situation
- Adb transport mode: check all special characters, handle them with care (escapes them properly), adbfs has [bugs that cause by this](https://github.com/spion/adbfs-rootless/issues/57)
- `madbfs` and `madbfs-msg`: Check tty, asks serial if only on tty, else hard error
- Fix bug: sometimes deleting files leave `.fuse_hiddenxxxxx` files (I don't know whether this is caused by outside process holding an open file handle or `madbfs` have some logic flaw or something)
  - Maybe add a check on `rename` operation that checks whether a file got renamed into a `.fuse_hiddenxxxxx` file, store the location into a kind of map, then at `madbfs` shutdown delete those files if still exists
  - Maybe add some flag that enable this functionality 
- Remove `@class`, `@enum`, etc fields from docstring, it's not necessary and sometimes, it's not synchronized
- Make truncation happen on the cache too
  - If a file is partially in the cache, when truncation happen, what should happen to the cache?
- `LruCache`: since `read()` and `write()` are launched in parallel for multiple pages, when lazily opening file, the operation may be duplicated. Fix: try to make "pending" open operation, something like `busy_queue`?
- `Page`: There might be a case when a write is done to existing file with seek. This leave a page with zeroes on the first N bytes. Then, when a read is performed on those first N bytes, the zeroes bytes are returned instead of the actual bytes on the file. There should be a flag that indicates that the data is not from the actual file on the device. This case only possible when the page was not retrieved from the file, but a write is performed. So a flag that indicates the page was indicative of data from the actual file might be a good idea.
  - An idea might be to flush (since dirty) written data on a page when a read is requested on that page. After that, we can fetch the missing data of that page, or read the entire page. I'm not sure which is better.
  - I need to make a test for this case as well...
  - I haven't even make a test case when a write is done with holes...
  - What if I use the dirty map itself as the indication of bytes synced to the files on device? The dirty flag signal that the map refers to unwritten data vs synced data?


# DONE

- ~Apparently mounting subdirectory can be done natively using fuse module subdir: `-o moddules=subdir,subdir=/path/to/subdirectory`. But, this mechanism is dumb, there's no validation whether the subdir exists or the subdir is a directory (it can be symlink for example)~
  > - apparently this module is kinda legacy?
  > - the dumbness is also not acceptable
  > - the module might not exists as well?
- When TTL is not zero and a directory is in actively changed, like copying a large directory, read/write may fail with "bad descriptor" error message: probably because the node is invalidated.
  > There are some missing stat update, so eventually the stat are unscynchronized and `update()` triggers
- IPC: allow failure from ipc handlers.
- Fix `--version` option returns 1 instead of 0
- Use better version tagging for in-progress development: `X.Y.Z-dev+g<hash>`
- Embed the server binaries
