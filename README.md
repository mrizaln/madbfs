# madbfs

`madbfs` (Modern `adb` Filesystem, formerly `adbfsm`) is a userspace filesystem for Android via `adb` built using `libfuse`. It focuses on being fast, reliable, and safe, with straightforward, well-structured code that follows modern C++ practices.

## Motivation

I want to manage my Android phone storage from my computer without using MTP (it's awful).

This project is inspired by the [`adbfs-rootless`](https://github.com/spion/adbfs-rootless) project. While `adbfs-rootless` works most of the time under light load it has reliability issues. Under high load (such as when a thumbnailer service is running) `adbfs-rootless` tends to crash, making it unusable until it is forcefully unmounted and its cache cleaned.

The file I/O approach of `adbfs-rootless` is also similar to that of MTP, in which each file is first pulled from device (using `adb pull`) before any operations like reading and writing performed. If a write operation is performed, the file is then pushed back into the device (using `adb push`). This approach limits the size of the file that can be handled before the cache is filled (it uses `/tmp`).

These limitations lead me to decide to rebuild the project from ground up in order to create a more stable, modern, and possibly faster solution.

## Features

> TLDR: full file and directory traversal with concurrent file streaming approach, partial read/write, and active caching.

- No root access required

  Non-rooted device will have standard file and directory access as regular user. Rooted device will have less constraints on file and directory access.

- Full file and directory traversal

  Browse the entire file tree of your Android device (subject to permission constraints).

- Read and write support

  Open, read, and write files (subject to permission constraints).

- Create and delete files and directories

  Support creating new files and directories as well as deleting them (subject to permission constraints).

- Rename and move

  Rename or move files and directory seamlessly (subject to permission constraints).

- Modify file timestamps

  Update file access and modification times.

- Automatic in-memory caching

  Recently accessed files are cached in memory using an LRU paging mechanism, allowing faster repeated access.

- Streamed file access (partial read/write)

  Read files on-demand without pulling the entire file first. Unlike MTP’s "pull-whole-file" approach, this filesystem streams data over `adb`, enabling efficient access to large files or specific file segments.

- Efficient resource use

  Loads only what you access, conserving memory usage and bandwidth. You can also control how much the cache stores file data.

- Concurrent file access

  Allows for concurrent access to files and directories without blocking. This ability comes out-of-the-box by virtue of using FUSE and `adb`.

- Flexible connection method

  `madbfs` offers two kinds of transport:

  - `adb` transport

    The simplest transport. It executes all FUSE operations by running `adb shell` commands like `dd`, `stat`, and `touch`. No additional component is required.

  - Proxy transport (optional)

    Communicates with a lightweight TCP server running natively on the Android device via a custom RPC protocol. It requires a server binary compiled for the phone architecture but has better I/O throughput than the `adb` transport.

  Both of the transport can work wired via USB or wireless in your local network. `madbfs` will automatically fall back to `adb` transport if the proxy transport is not available.

- Resilient to disconnections

  Stays mounted even when the device disconnects. Cached files and directories remain accessible, and full functionality resumes automatically when the connection is restored.

- Built with modern C++

  Developed in C++23 using coroutine-style asynchronous programming for clean, lightweight, high-performance I/O.

## Dependencies

- `madbfs`

  Runtime dependencies

  - adb

  Build dependencies

  - GCC or Clang (support C++23 or above, tested on GCC 14, GCC 15, and Clang 20 )
  - CMake (version 3.22+)
  - Conan

  Library dependencies

  - Boost (Asio, Process, and JSON component)
  - fmt
  - libfuse
  - rapidhash
  - spdlog

- `madbfs-server` (optional)

  Build dependencies

  - Android NDK (support C++23 or above, tested on NDK version 29 (beta 1))
  - CMake (version 3.22+)
  - Conan

  Library dependencies

  - Asio (non-Boost variant)
  - fmt
  - spdlog

## Building

> if you just want a prebuilt binaries, jump to [installation](#installation)

Since the dependencies are managed by Conan, you need to make sure it's installed and configured correctly (consult the documentation on how to do it, [here](https://docs.conan.io/2/installation.html)) on your system.

> don't forget to run `conan profile detect` if you use Conan for the first time

- `madbfs`

  Navigate to the root of the repository, then you install the dependencies:

  ```sh
  conan install . --build=missing -s build_type=Release
  ```

  > - you might want to change the Conan profile by appending `-pr <profile_path>` to the command above.
  > - for an example, you can see [this profile I use for CI](./.github/ci/conan-profile.ini) (you can also just use this one)

  Then compile the project:

  ```sh
  cmake --preset conan-release
  cmake --build --preset conan-release
  ```

  The built binary will be in `build/Release/madbfs/` directory with the name `madbfs`. Since the libraries are statically linked to the binary you can place this binary anywhere you want (place it in PATH if you want it to be accessible from anywhere).

- `madbfs-server`

  > you may skip this process if you don't mind not having proxy transport support

  Navigate to the root of the repository then go to `madbfs-server/` subdirectory. You then can proceed by editing the [`build_all.sh`](./madbfs-server/build_all.sh) script. This step is necessary to set the Android native app build system and its parameters to be able to compile the server. You may change these parameters

  - API_LEVEL
    > see [this](https://apilevels.com/)
  - COMPILER
    > usually clang
  - COMPILER_VERSION
    > not ndk version! (check by running the compiler on the NDK path)

  Your `ANDROID_NDK_HOME` variable must be set before compiling and point to the appropriate Android NDK. If your `ANDROID_NDK_HOME` variable is not set, you can set it yourself like so before going to the next step:

  ```sh
  export ANDROID_NDK_HOME=/path/to/your/android/ndk/home/
  ```

  The compilation step is simpler:

  ```sh
  ./build_all.sh
  ```

  The script above will build the server for all the currently supported Android ABIs (see [this](https://developer.android.com/ndk/guides/abis)). The binaries will be built in its own `build/android-<arch>-release` directory. A stripped version of all the binaries are copied to `build/android-all-release` directory.

- Package

  To allow for easier packaging, I have created [a script](./package.sh) that compiles both `madbfs` and `madbfs-server` in one go:

  ```sh
  ./package.sh
  ```

  > - you must configure the Android NDK variables first on [`build_all.sh`](./madbfs-server/build_all.sh) script and set the `ANDROID_NDK_HOME` environment variable before using this script.
  > - you can given an argument to this script to set your Conan profile for building the `madbfs` client.

  This will build and package `madbfs` into a `tar.gz` file in `build/package/` directory.

## Installation

You can download `madbfs` from the [GitHub releases page](https://github.com/mrizaln/madbfs/releases).

There is no installation step required, as the application is built statically. You can place the binaries wherever you prefer. However, make sure to place the server binaries (`madbfs-server-*`) in the same directory as `madbfs` (the client), or else `madbfs` won't be able to find the server and will fall back to using the `adb` transport.

If you separate `madbfs` from the server binaries, you need to specify them yourself using `--server` option (explained in the next section). Doing this also means that you need to know your Android device ABI beforehand to select the correct server binary. Failing to do so will result in the server not running and the client will fall back to using the `adb` transport.

## Usage

The help message can help you start using this program

```
usage: madbfs [options] <mountpoint>

Options for madbfs:
    --serial=<str>         serial number of the device to mount
                             (you can omit this [detection is similar to adb])
                             (will prompt if more than one device exists)
    --server=<path>        path to server file
                             (if omitted will search the file automatically)
                             (must have the same arch as your phone)
    --log-level=<enum>     log level to use
                             (default: "warning")
                             (enum: "trace", "debug", "info", "warning", "error", "critical", "off")
    --log-file=<path>      log file to write to
                             (default: "-" for stdout)
    --cache-size=<int>     maximum size of the cache in MiB
                             (default: 256)
                             (minimum: 128)
                             (value will be rounded up to the next power of 2)
    --page-size=<int>      page size for cache & transfer in KiB
                             (default: 128)
                             (minimum: 64)
                             (value will be rounded up to the next power of 2)
    --ttl=<int>            set the TTL of the stat cache of the filesystem in seconds
                             (default: 30)
                             (set to 0 to disable it)
    --timeout=<int>        set the timeout of every remote operation
                             (default: 10)
                             (set to 0 to disable it)
    --port=<int>           set the port number the server will listen on
                             (default: 12345)
    --no-server            don't launch server
                             (will still attempt to connect to specified port)
                             (fall back to adb shell calls if connection failed)
                             (useful for debugging the server)

Options for libfuse:
    -h   --help            print help
    -V   --version         print version
    -d   -o debug          enable debug output (implies -f)
    -f                     foreground operation
    -s                     disable multi-threaded operation
    -o clone_fd            use separate fuse device fd for each thread
                           (may improve performance)
    -o max_idle_threads    the maximum number of idle worker threads
                           allowed (default: -1)
    -o max_threads         the maximum number of worker threads
                           allowed (default: 10)
    -o allow_other         allow access by all users
    -o allow_root          allow access by root
    -o auto_unmount        auto unmount on process termination
```

### Unmounting

To unmount the filesystem, you run this command like for any other FUSE filesystem:

```sh
fusermount -u <mountpoint>
```

### Selecting device

To mount your device you only need to specify the mount point if there is only one device. If there are more than one device then you can specify the serial using `--serial` option. If you omit the `--serial` option when there are multiple device connected to the computer, you will be prompted to specify the device you want to mount.

```sh
$ ./madbfs mount
[madbfs] checking adb availability...
[madbfs] multiple devices detected,
         - 1: 068832516O101622
         - 2: 192.168.240.112:5555
[madbfs] please specify which one you would like to use: _
```

`madbfs` respects the env variable `ANDROID_SERIAL` (mimicking `adb` behavior) so you can alternately use it to specify the device.

```sh
$ ANDROID_SERIAL=068832516O101622 ./madbfs
[madbfs] checking adb availability...
[madbfs] using serial '068832516O101622' from env variable 'ANDROID_SERIAL'
```

### Specifying server and port number

> only relevant if you want proxy transport support

In order to use the proxy transport, `madbfs` needs to be able to find the `madbfs-server` binary. There are three approaches you can do in order for `madbfs` be able to find the server file:

- Place it where you run the `madbfs` program,
- Place it in the same directory as `madbfs` program, or
- Specify explicitly the path of the file using `--server` option.

If you want the filesystem to use `adb` transport instead then you can use `--no-server` flag. This flag prevents `madbfs` from pushing the server into your phone and running it.

The proxy communicates with `madbfs` over TCP enabled by port forwarding and by default it will listen on port `12345`. If you find this port to be not suitable for your use you can always specify it with `--port` option.

### Cache size

`madbfs` caches all the read/write operations on the files on the device. This cache is stored in memory. You can control the size of this cache using `--cache-size` option (in MiB). The default value is `256` (256 MiB).

```sh
$ ./madbfs --cache-size=256 <mountpoint>    # 256 MiB of memory will be used as file cache
```

### Page size

In the cache, each file is divided into pages. The `--page-size` option dictates the size of this page (in KiB). Page size also dictates the size of the buffer used to read/write into the file on the device. You can adjust this value according to your use.

```sh
$ ./madbfs --page-size=128 <mountpoint>    # read/write operations are communicated in 128 KiB chunks

```

### Logging

The default log file is stdout (specified by "-"; which goes to nowhere when not run in foreground mode). You can manually set the log file using `--log-file` option and set the log level using `--log-level`.

```sh
$ ./madbfs --log-file=madbfs.log --log-level=debug <mountpoint>
```

### Debug mode

As part of debugging functionality `libfuse` has provided debug mode through `-d` flag. You can use this to monitor `madbfs` operations (if you don't want to use log file or want to see the log in real-time). If the debugging information is too verbose, you can use `-f` instead to make madbfs run in foreground mode without printing `fuse` debug information.

```sh
$ ./madbfs --log-file=- --log-level=debug -f <mountpoint>                     # runs in foreground (not daemonized)
$ ./madbfs --log-file=- --log-level=debug -d <mountpoint>                     # this will print the libfuse debug messages and madbfs log messages
$ ./madbfs --log-file=- --log-level=debug -d <mountpoint> 2> /dev/null        # this will print only madbfs log messages since libfuse debug messages are printed to stderr
```

### IPC (and `madbfs-msg`)

Filesystem parameters can be reconfigured and queried during runtime though IPC using unix socket. The supported operations are:

- help,
- version,
- info,
- invalidate cache,
- set page size,
- set cache size,
- set ttl,
- set timeout,
- set log level, and
- logcat (read `madbfs` log in real-time, similar to `adb logcat`).

The address of the socket in which you can connect to as client is composed of the name of the filesystem and the serial of the device. The socket itself is created in directory defined by `XDG_RUNTIME_DIR` environment variable (it's usually set to `/run/user/<uid>`). If the `XDG_RUNTIME_DIR` is not defined, as fallback, the directory is set to `/tmp`. The socket will be created when the filesystem initializes.

For example, the socket path for a device with serial `192.168.240.112:5555`:

```
/run/user/1000/madbfs@192.168.240.112:5555.sock
```

If at initialization this socket file exists, the IPC won't start. This may happen if the filesystem is terminated unexpectedly (crash or kill signal). You need to remove this file manually if that happens.

For the specification of the message protocol used on the IPC and how to use it read [IPC.md](IPC.md) file. To make it easier for user to use the IPC without having to write their own socket code, I have created another executable: `madbfs-msg`. The possible operations are explained in [IPC.md](IPC.md) file as well.

## Benchmark

Benchmark is done by writing a 64 MiB file using `dd` and then reading it back. The statistics printed by `dd` is used for the speed value so is for `adb push` and `adb pull`. The test is done on an Android 11 phone (armv8) using USB cable with proxy transport. As baseline, the speed on which an `adb push` (write) and an `adb pull` (read) operation is done on a file with the same size is measured. `madbfs` is launched using its default parameters (cache size = 256 MiB, page size = 128 KiB).

The write command is as follows:

```sh
  dd if=/dev/random of=random bs=128K count=1024
```

The read command is as follows:

```sh
  dd if=random of=/dev/null bs=128K count=1024
```

### Proxy transport

- Write

  | operation | speed (MB/s) | relative speed |
  | :-------- | -----------: | -------------: |
  | adb push  |         15.3 |           1.00 |
  | write     |         13.1 |           0.86 |

- Read

  | operation       | speed (MB/s) | relative speed |
  | :-------------- | -----------: | -------------: |
  | adb pull        |         11.7 |           1.00 |
  | read (no cache) |         10.8 |           0.93 |
  | read (cache)    |       2100.0 |         179.49 |

### `adb` transport

- Write

  | operation | speed (MB/s) | relative speed |
  | :-------- | -----------: | -------------: |
  | adb push  |         15.3 |           1.00 |
  | write     |         12.4 |           0.81 |

- Read

  | operation       | speed (MB/s) | relative speed |
  | :-------------- | -----------: | -------------: |
  | adb pull        |         11.7 |           1.00 |
  | read (no cache) |          3.6 |           0.24 |
  | read (cache)    |       2100.0 |         179.49 |

## TODO

- [ ] Cache the file handle for read/write instead of opening the file each read/write operation.
- [ ] Implement proper permission check (this is difficult since I need to open the file to actually check access, `mode_t` value is not sufficient).
- [ ] Improve reconnection logic.
- [ ] Rework RPC framing to be able to recover from error cleanly.
- [x] Eliminate copying data to and from memory when transferring/copying files within the filesystem.
- [x] IPC to talk to the `madbfs` to control the filesystem parameters like invalidation, timeout, cache size, etc.
- [x] Implement file read and write operation caching in memory.
- [x] Implement the filesystem as actual tree for caching the stat.
- [x] Implement versioning on each node that expires every certain period of time. When a node expires it needs to query the files from the device again.
- [x] Make the codebase async using C++20 coroutines.
- [x] Periodic cache invalidation. Current implementation only look at the size of current cache and only invalidate oldest entry when newest entry is added and the size exceed the `cache-size` limit.
- [x] Use persistent TCP connection to the server instead of making connection per request.
