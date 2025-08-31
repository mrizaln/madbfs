# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.8.1] - 2025-08-31

### Fixed

- Flushing may fail on file with certain size and/or slow write speed.

## [0.8.0] - 2025-08-27

### Added

- CI workflow.
- Benchmark on README.md
- `info` IPC operation.

### Fixed

- Crash when symlink target doesn't have access permission.
- ABI query at startup fail when there is more than one device.
- `utimens` operation on `adb` connection.
- Fuse header includes points to system header instead (which may not exist) instead of Conan.
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

[unreleased]: https://github.com/mrizaln/madbfs/compare/v0.8.1...HEAD
[0.8.1]: https://github.com/mrizaln/madbfs/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/mrizaln/madbfs/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/mrizaln/madbfs/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mrizaln/madbfs/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/mrizaln/madbfs/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/mrizaln/madbfs/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/mrizaln/madbfs/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/mrizaln/madbfs/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mrizaln/madbfs/releases/tag/v0.1.0
