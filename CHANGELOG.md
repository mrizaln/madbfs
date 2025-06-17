# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

- Make `read`/`write` parallelize-friendly.
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

[unreleased]: https://github.com/mrizaln/madbfs/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/mrizaln/madbfs/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/mrizaln/madbfs/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/mrizaln/madbfs/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/mrizaln/madbfs/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/mrizaln/madbfs/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mrizaln/madbfs/releases/tag/v0.1.0
