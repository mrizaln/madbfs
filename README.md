# madbfs

This project, `madbfs` (modern adb filesystem, formerly `adbfsm`), aims to create a well-built filesystem abstraction over `adb` using `libfuse` that is fast, safe, and reliable while also have have well structured code according to modern C++ (20 and above) practices.

## Motivation

I want to manage my Android phone storage from my computer without using MTP (it's awful).

This project is inspired by the [`adbfs-rootless`](https://github.com/spion/adbfs-rootless) project by Spion, available on GitHub. While `adbfs-rootless` works as intended, I encoutered frequent crashes that affected its reliability. I initially considered contributing fixes directly into the codebase, but found it somewhat dated with practices that don't align well with modern practices. Consequently, I decided to rebuild the project from the ground up to create a more stable and modern solution.

## Features

> TLDR: Full file and directory traversal with concurrent file streaming approach, partial read/write, and active caching.

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

- FUSE integration

  Seamlessly mount your Android device as a regular filesystem. Fully compatible with standard tools like `ls`, `cp`, `mv`, `cat`, `vim`, etc.

- Automatic in-memory caching

  Recently accessed files are cached in memory using an LRU paging mechanism, allowing faster repeated access.

- Streamed file access (partial read/write)

  Read files on-demand without pulling the entire file first. Unlike MTP’s "pull-whole-file" model, this filesystem streams data over `adb`, enabling efficient access to large files or specific file segments.

- Efficient resource use

  Loads only what you access, conserving memory usage and bandwidth. You can also control how much the cache stores file data.

- Concurrent file access

  Allows for concurrent access to files and directories without blocking. This ability comes out-of-the-box by virtue of using FUSE and `adb`.

- Flexible connection method

  `madbfs` offers two kinds of transport:

  - `adb` transport

    The simplest transport. It executes all FUSE operations by running `adb shell` commands like `dd`, `stat`, and `touch`. No additional component is required.

  - Proxy transport (optional)

    Communicates with a lightweight TCP server running natively on the Android device via a custom RPC protocol. It requires a server binary compiled for the phone architecture but has better i/o throughput than the `adb` transport.

  Both of the transport can work wired via USB or wireless in your local network. `madbfs` will automatically fall back to `adb` transport if the server is not available.

- Resilient to disconnections

  Stays mounted even when the device disconnects. Cached files and directories remain accessible, and full functionality resumes automatically when the connection is restored.

- Built with modern C++

  Developed in C++23 using coroutine-style asynchronous programming for clean, lightweight, high-performance I/O.

## Dependencies

- `madbfs`

  Build dependencies

  - CMake
  - Conan

  Library dependencies

  - Boost (Asio, Process, and JSON component)
  - fmt
  - libfuse
  - rapidhash
  - spdlog

- `madbfs-server` (optional)

  Build dependencies

  - Android NDK (must support C++20 or above)
  - CMake
  - Conan

  Library dependencies

  - Asio (non-Boost variant)
  - fmt
  - spdlog

## Building

Since the dependencies are managed by Conan, you need to make sure it's installed and configured correctly (consult the documentation on how to do it, [here](https://docs.conan.io/2/installation.html)) on your system.

> don't forget to run `conan profile detect` if you use Conan for the first time

- `madbfs`

  Navigate to the root of the repository, then you install the dependencies:

  ```sh
  conan install . --build=missing -s build_type=Release
  ```

  Then compile the project:

  ```sh
  cmake --preset conan-release
  cmake --build --preset conan-release
  ```

  The built binary will be in `build/Release/madbfs/` directory with the name `madbfs`. Since the libraries are statically linked to the binary you can place this binary anywhere you want (place it in PATH if you want it to be accessible from anywhere).

- `madbfs-server`

  > you may skip this process if you don't mind not having proxy transport support

  Navigate to the root of the repository then go to `madbfs-server/` subdirectory. You then can proceed by editing the `conan-android-profile.conf` file. This step is necessary to set the Android native app build system and its paramater to better suit your Android device(s). The parameters that you may change are

  - Android NDK path (`tools.android:ndk_path`),
  - Android ABI (`arch`),
  - Android API level (`os.api_level`), and
  - Compiler version (`compiler.version`).

  The dependencies installation step is similar to previous one with some additional flags to the command like so:

  ```sh
  conan install . --build missing -s build_type=Release --profile:build default --profile:host conan-android-profile.conf
  ```

  The compilation process is the same:

  ```sh
  cmake --preset conan-release
  cmake --build --preset conan-release
  ```

  The binary will be in `build/Release/` directory relative to `madbfs-server/` with the name `madbfs-server`. You may want to place `madbfs-server` in the same directory as `madbfs` to allow `madbfs` find it automatically when you run it unless you don't mind running `madbfs` with `--server` flag (explained in the next section).

## Usage

The help message can help you start using this program

```
usage: madbfs [options] <mountpoint>

Options for madbfs:
    --serial=<s>         serial number of the device to mount
                           (you can omit this [detection is similar to adb])
                           (will prompt if more than one device exists)
    --server             path to server file
                           (if omitted will search the file automatically)
                           (must have the same arch as your phone)
    --log-level=<l>      log level to use (default: warn)
    --log-file=<f>       log file to write to (default: - for stdout)
    --cache-size=<n>     maximum size of the cache in MiB
                            (default: 256)
                           (minimum: 128)
                           (value will be rounded to the next power of 2)
    --page-size=<n>      page size for cache & transfer in KiB
                           (default: 128)
                           (minimum: 64)
                           (value will be rounded to the next power of 2)
    --port=<n>           set port the server listens on
                           (default: 12345)
    --no-server          don't launch server
                           (will still attempt to connect to specified port)
                           (fall back to adb shell calls if connection failed)
                           (useful for debugging the server)
    -h   --help          show this help message
    --full-help          show full help message (includes libfuse options)
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

In order to use the proxy transport, `madbfs` needs to be able to find the `madbfs-server` binary. There are three approaches you can do in order for `mabfs` be able to find the server file:

- Place it where you run the `madbfs` program,
- Place it in the same directory as `madbfs` program, or
- Specify explicitly the path of the file using `--server` flag.

If you want the filesystem to use `adb` transport instead then you can use `--no-server` flag. This flag prevents `madbfs` from pushing the server into your phone and running it.

The proxy runs communicates with `madbfs` over TCP enabled by port forwarding and by default it will listen on `12345` port number. If you find this port to be not suitable for your use you can always specify it with `--port` flag.


### Cache size

`madbfs` caches all the read/write operations on the files on the device. This cache is stored in memory. You can control the size of this cache using `--cache-size` option (in MiB). The default value is `256` (256MiB).

```sh
$ ./madbfs --cache-size=256 <mountpoint>    # using 256MiB of cache
```

### Page size

In the cache, each file is divided into pages. The `--page-size` option dictates the size of this page (in KiB). Page size also dictates the size of the buffer used to read/write into the file on the device. You can adjust this value according to your use. From my testing, `page-size` of value `128` (means 128KiB) works well when using USB cable for the `adb` connection. You may want to decrease or increase this value for your use case. The default value is `128` (128KiB).

```sh
$ ./madbfs --page-size=128 <mountpoint>    # using 128KiB of page size

```

### Logging

The default log file is stdout (which goes to nowhere when not run in foreground mode). You can manually set the log file using `--log-file` option and set the log level using `--log-level`.

```sh
$ ./madbfs --log-file=madbfs.log --log-level=debug <mountpoint>
```

### Debug mode

As part of debugging functionality `libfuse` has provided debug mode through `-d` flag. You can use this to monitor `madbfs` operations (if you don't want to use log file or want to see the log in real-time). If the debugging information is too verbose, you can use `-f` instead to make madbfs run in foreground mode without printing `fuse` debug information.

```sh
$ ./madbfs --log-file=- --log-level=debug -d <mountpoint>                     # this will print the libfuse debug messages and madbfs log messages
$ ./madbfs --log-file=- --log-level=debug -d <mountpoint> 2> /dev/null        # this will print only madbfs log messages since libfuse debug messages are printed to stderr
```

### IPC

> see [this python script](./madbfs/test/ipc.py) for an example of an IPC client.

Filesystem parameters can be reconfigured and queried during runtime though IPC using unix socket. The supported operations are:

- help,
- invalidate cache,
- set/get page size, and
- set/get cache size.

The address of the socket in which you can connect to as client is composed of the name of the filesystem and the serial of the device. The socket itself is created in directory defined by `XDG_RUNTIME_DIR` environment variable (it's usually set to `/run/user/<uid>`). If the `XDG_RUNTIME_DIR` is not defined, as fallback, the directory is set to `/tmp`. The socket will be created when the filesystem initializes.

For example, the socket path for a device with serial `192.168.240.112:5555`:

```
/run/user/1000/madbfs@192.168.240.112:5555.sock
```

If at initialization this socket file exists, the IPC won't start. This may happen if the filesystem is terminated unexpectedly (crash or kill signal). You need to remove this file manually if that happens.

The communication though the IPC is done using a simple Length-Value message protocol. The first 4 bytes of the message is the length of it (excluding itself) in network byte order, and the rest is the payload.

The payload must be a JSON object in the form that depends on the operation requested. The general form of the JSON is in the form of:

```json
{
  "op": <name>,
  "value": <value>
}
```

Some operations only requires `"op"` field, while some requires `"value"` field. Below is the break down:

- Help

  ```json
  { "op": "help" }
  ```

- Invalidate cache:

  ```json
  { "op": "invalidate_cache" }
  ```

- Get cache size

  ```json
  { "op": "get_cache_size" }
  ```

- Get page size

  ```json
  { "op": "get_page_size" }
  ```

- Set cache size:

  ```json
  { "op": "set_cache_size", "value": { "mib": <uint> } }
  ```

  > - uint must be greater than or equal to 128
  > - the value will be rouded up to the nearest multiple of 2

- Set page size:

  ```json
  { "op": "set_page_size", "value": { "kib": <uint> } }
  ```

  > - uint must be between 64 and 4096
  > - the value will be rouded up to the nearest multiple of 2

The IPC will reply immediately after an operation complete. The reply is in a JSON in the form of

```json
{
  "status", <success|error>,
  "<value|message>": <value>
}
```

The second field will be `"value"` if the `"status"` is `"success"`, else the second field will be `"message"` if the `"status"` is `"error"`.

The `<value>` then will be different depending on the operation performed:

- Invalidate cache:

  ```json
  { "status": "success", "value": null }
  ```

- Get cache size

  ```json
  { "status": "success", "value": <uint> }
  ```

- Get page size

  ```json
  { "status": "success", "value": <uint> }
  ```

- Set cache size:

  ```json
  {
    "status": "success",
    "value": {
      "old_cache_size": <old_value>,
      "new_cache_size": <new_value>
    }
  }
  ```

  > size is in MiB

- Set page size:

  ```json
  {
    "status": "success",
    "value": {
      "old_cache_size": <old_value>,
      "new_cache_size": <new_value>,
      "old_page_size": <old_value>,
      "new_page_size": <new_value>,
    }
  }
  ```

  > cache size is in MiB
  > page size is in KiB

## TODO

- [x] Make the codebase async using C++20 coroutines.
- [x] IPC to talk to the `madbfs` to control the filesystem parameters like invalidation, timeout, cache size, etc.
- [x] Implement the filesystem as actual tree for caching the stat.
- [x] Implement file read and write operation caching in memory.
- [x] ~~Implement proper multithreading, (not needed, since it's using async now, though multiple executor might help).~~
- [ ] Implement proper permission check.
- [ ] Implement versioning on each node that expires every certain period of time. When a node expires it needs to query the files from the device again.
- [ ] Periodic cache invalidation. Current implementation only look at the size of current cache and only invalidate oldest entry when newest entry is added and the size exceed the `cache-size` limit.
- [x] Eliminate copying data to and from memory when transferring/copying files within the filesystem.
- [ ] Use multiple threads backing the async runtime.
- [ ] Rewrite the server app in Kotlin, using the Android runtime instead as native binary.
- [x] Use persistent TCP connection to the server instead of making connection per request.
- [ ] Fix open/close semantics.
- [ ] Add limit to open file descriptor (for adb query it using `ulimit -n`, for server query it using `getrlimit`)
