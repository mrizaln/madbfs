# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.11.0] - 2026-06-11

### Added

- Storage for file handles to enable faster `Node` retrieval on read/write using its associated `fh` (no need to traverse the tree).
- No-cache mode that is an approximation for direct I/O. File content cache is not enabled in this mode and operations use real file `fd` from device. File stat is still cached though.
- Mounting subdirectory feature (custom root) (https://github.com/mrizaln/madbfs/issues/18).
- Program option for using no-cache mode (`--no-cache`).
- Program option for mounting subdirectory (`--root`).
- New field on IPC `info` operation: `root` (the root the filesystem uses for the subdirectory mounting feature).
- New field on IPC `info` operation: `serial` (serial of the connected device as seen by `adb`).

### Changed

- Rename `FileTree` to `Filesystem`.
- `Filesystem` owns `Cache` now.
- Move all operations logic from `Node` to `Filesystem`, this reduces maintenance burden and reduces indirection.
- `Link` is lazy now, the target will only be read on first access.
- Common connection errors are not cached now: `std::errc::not_connected` (`ENOTCONN`), `std::errc::timed_out` (`TIMEDOUT`), and `std::errc::resource_unavailable_try_again` (`EAGAIN`).
- Allow non-connection failure on IPC responses.
- IPC operations `invalidate_cache`, `set_page_size`, and `set_cache_size` will respond with failure (unix error message, check `errno(3)`) if invoked when using no-cache mode.
- Response value for IPC `info` operation is changed: `page_cache` and `cache_size` fields are now under `cache` field which may be `null` if no-cache mode is used.
- Change IPC `info` operation response field, `connection` to `transport` for consistency.
- Return `std::errc::invalid_value` (`EINVAL`) instead of `std::errc::operation_not_supported` (`EOPNOTSUPP`) when FUSE operations is provided with malformed path.

### Fixed

- Missing `Stat` update on `mknod()`, `mkdir()`, `unlink()`, `rmdir()`, `rename()`, and `copy_file_range()`.
- Flush on eviction uses incorrect function that doesn't reset dirty flag on evicted page.
- Rename operation doesn't update `fd` map on `adb` transport mode.

## [0.10.1] - 2026-04-17

### Added

- IPC operation for forcing node stats expiration (`expire_stat`).

### Changed

- Changing TTL now resets the expiration of the files if the new TTL is different from the old one.
- Default TTL is now set to 60 seconds instead of 30 seconds.

### Fixed

- Buffer use-after-move for request handler (server).

## [0.10.0] - 2026-04-17

### Added

- Better signal handling using ASIO mechanisms.
- IPC operation for unmounting filesystem (`unmount`).
- `Ping` procedure that checks connection every once in a while.
- Proper reconnection logic across any transport method.
- Make connection able to change the transport to other one available if current one is not responding or has an error.
- Program option `--adb-only` to set the server to use `adb` transport only.

### Fixed

- Partial write on `copy_file_range` operation not implemented correctly.
- `rename` handler on `proxy` mode doesn't have any fallback on unavailable `renameat2` syscall.
- Using bad date format for `utimens` operation on `adb` connection.
- `stat::st_blocks` calculation (https://github.com/mrizaln/madbfs/issues/14).

### Changed

- Read and write operation now open a cached file descriptor for repeated operations instead of opening the file every read/write.
- `logcat` entries/messages now no longer contain any newline and are now aligned.
- Connection type on IPC `info` operation for `server` is renamed to `proxy` (it's should have been this way from the beginning).
- Change the default port number from `12345` to `23237` (`adbfs` on dial pad).
- Change the default timeout from 10 seconds to 2 seconds.

## [0.9.0] - 2025-09-28

### Added

- Stat cache expiration (TTL).
- Program option for TTL (`--ttl`).
- Program option timeout (`--timeout`).
- IPC operation for setting TTL (`set_ttl`).
- IPC operation for setting timeout (`set_timeout`).
- IPC operation for setting log level (`log_level`).
- IPC operation for getting `madbfs` version (`version`).
- Logcat operation through IPC (`logcat`) which can be used to read `madbfs` log in real-time.
- New `madbfs-msg` binary/subproject as default IPC client.

### Fixed

- Server responds with out of order overlapping response.
- Faulty single file cache invalidation.
- Partial write on `copy_file_range` operation not handled.

### Changed

- Symlink now handled correctly (previously symlink works by taking the `realpath` of the link repeatedly until it points to a non-symlink file and can only points to valid target).
- Arguments options `--cache-size`, `--page-size`, and `--port` won't accept non-positive value.
- Improve `Path` and `PathBuf` API and implementation.
- Add new `ttl`, `timeout`, and `log_level` fields on `info` IPC operation response.

### Removed

- Defer macro.

## [0.8.1] - 2025-08-31

### Fixed

- Flushing may fail on file with certain size and/or slow write speed.

### Changed

- Revert parallel `flush` operation.

## [0.8.0] - 2025-08-27

### Added

- CI workflow.
- Benchmark on README.md
- `info` IPC operation.

### Fixed

- Crash when symlink target doesn't have access permission.
- ABI query at startup fail when there is more than one device.
- `utimens` operation on `adb` connection.
- Fuse header includes points to system header (which may not exist) instead of Conan.
- `stat::st_blocks` calculation.

### Changed

- Use channel to synchronize writing on socket.
- Make multiple adjacent page `flush` operation launch in parallel.
- Make all IPC response cleaner.

### Removed

- `get_cache_size` IPC operation.
- `get_page_size` IPC operation.

## [0.7.0] - 2025-06-26

### Added

- Build script for server for all supported Android ABIs.
- Package script for `madbfs` and its servers.

### Fixed

- Unnecessary `find_package` command for `unordered_dense`.
- Functions in `madbfs::operations::operations` not marked as `noexcept`.
- File size info not set correctly for directories.
- Conversion errors on server code.

### Changed

- Use `std::atomic` and `std::optional` instead of `std::future` and `std::promise` on sync-async bridge.
- Server discovery logic now considers target device ABI.

## [0.6.0] - 2025-06-17

### Added

- Filesytem capability and correctness test.

### Fixed

- Add procedure validity check on RPC payload parsing.
- `open` operation on a file with `O_TRUNC` flag won't truncate said file.
- Coroutine lambda on `rpc::Client::start` not live long enough for resumption which leads to use-after-free bug.
- Logger not flushed correctly at the end of program.
- `truncate`, `unlink`, and `rename` operations not handled on `Cache`.
- Server file discovery logic not working as intended.
- Logging functions' template argument deduction not working.

### Changed

- Make multiple adjacent page `read` and `write` operation launch in parallel.
- Lower the default cache size to `256` MiB from `512` MiB.

### Removed

- `unordered_dense` dependency.

## [0.5.0] - 2025-06-05

### Added

- Handshake function for client and server.
- Reconnection logic for `ServerConnection`.

### Fixed

- Can't `rmdir` a directory if it contains an `Error` node.

### Changed

- Handle additional arguments on `mknod`, `mkdir`, and `rename` operations.
- Move `spdlog`, `fmt`, and `saf` dependencies to `madbfs-common` submodule.
- Move `log.hpp` to `madbfs-common` submodule.
- Reimplement RPC to use persistent connection.

## [0.4.0] - 2025-05-31

### Added

- `copy_file_range` operation.
- Proxy transport, implemented as a TCP server that runs on Android device, as better alternative to pure `adb shell` commands for `Connection`.
- `ServerConnection` that abstracts the connection with TCP server.
- Custom RPC for communication between TCP server and `madbfs`.
- New program arguments controlling server.
- `rename` function on `Path`.

### Fixed

- Prompt's error state not handled correctly.
- Apply rule of five to non-trivial classes.

### Changed

- Use `set` for directory entries instead of `vector`.
- Rename `rm` operation to `unlink`.
- Rename `mv` operation to `rename`.
- Rename interface `IConnection` to `Connection`.
- Rename `Connection` to `AdbConnection`.
- Make `Cache` holds a reference to `Connection` for independent flushing.

## [0.3.0] - 2025-05-18

### Added

- `saf` (scheduler-aware future) for read/write synchronization and read requests deduplication.

### Changed

- Rename project from `adbfsm` to `madbfs`.
- Make the filesystem asynchronous using `Asio` (Boost variant) as its runtime.
- Replace `reproc` with `Boost.Process`.
- Replace `nlohmann_json` with `Boost.JSON`.

### Removed

- `shared_futex` dependency.

## [0.2.0] - 2025-05-15

### Added

- IPC for configuring filesystem parameters

### Fixed

- Add a check for no device found error.
- Structural mutex not held on file operations.

### Changed

- Use proper mutex for file access instead of atomics.
- Use `find` for `getattr` (and `readdir`) operation instead of `ls` to get better stat result.

## [0.1.0] - 2025-05-05

### Added

- `FileTree`, a trie, to simulate the filesystem.
- `Node` of `FileTree` that store files metadata, effectively caching it.
- Cross-file LRU page caching for file read/write.
- `Path` and `PathBuf` class to separate `madbfs`'s virtual paths from real paths (`std::filesystem`).

[unreleased]: https://github.com/mrizaln/madbfs/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/mrizaln/madbfs/compare/v0.10.1...v0.11.0
[0.10.1]: https://github.com/mrizaln/madbfs/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/mrizaln/madbfs/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/mrizaln/madbfs/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/mrizaln/madbfs/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/mrizaln/madbfs/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/mrizaln/madbfs/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mrizaln/madbfs/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/mrizaln/madbfs/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/mrizaln/madbfs/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/mrizaln/madbfs/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/mrizaln/madbfs/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mrizaln/madbfs/releases/tag/v0.1.0
