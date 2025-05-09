# adbfsm

The name of the project can be separated as `adb`, `fs`, and `m` which means `adb filesystem modern`.

This project aims to create a well-built filesystem abstraction over `adb` using `libfuse` that is fast, safe, and reliable while also have have well structured code according to modern C++ (20 and above) practices.

## Motivation

I want to manage my Android phone storage from my computer without using MTP (it's awful).

This project is inspired by the [`adbfs-rootless`](https://github.com/spion/adbfs-rootless) project by Spion, available on GitHub. While `adbfs-rootless` works as intended, I encoutered frequent crashes that affected its reliability. I initially considered contributing fixes directly into the codebase, but found it somewhat dated with practices that don't align well with modern practices. Consequently, I decided to rebuild the project from the ground up to create a more stable and modern solution.

## Dependencies

Build dependencies

- CMake
- Conan

Library dependencies

- fmt
- libfuse
- rapidhash
- reproc
- spdlog

## Building

Since the dependencies are managed by Conan, you need to make sure it's installed and configured correctly (consult the documentation on how to do it, [here](https://docs.conan.io/2/installation.html)) on your system.

> don't forget to run `conan profile detect` if you use Conan for the first time

First you install the dependencies

```sh
conan install . --build=missing -s build_type=Release
```

Then compile the project

```sh
cmake --preset conan-release
cmake --build --preset conan-release
```

The built binary will be in `build/Release/` directory with the name `adbfsm`. You can place this binary anywhere you want (place it in PATH if you want it to be accessible from anywhere).

## Usage

The help message can help you start using this program

```
usage: adbfsm [options] <mountpoint>

Options for adbfsm:
    --serial=<s>         serial number of the device to mount
                           (you can omit this [detection is similar to adb])
                           (will prompt if more than one device exists)
    --log-level=<l>      log level to use (default: warn)
    --log-file=<f>       log file to write to (default: - for stdout)
    --cache-size=<n>     maximum size of the cache in MiB
                           (default: 512)
                           (minimum: 128)
                           (value will be rounded to the next power of 2)
    --page-size=<n>      page size for cache & transfer in KiB
                           (default: 128)
                           (minimum: 64)
                           (value will be rounded to the next power of 2)
    -h   --help          show this help message
    --full-help          show full help message (includes libfuse options)
```

### Selecting device

To mount your device you only need to specify the mount point if there is only one device. If there are more than one device then you can specify the serial using `--serial` option. If you omit the `--serial` option when there are multiple device connected to the computer, you will be prompted to specify the device you want to mount.

```sh
$ ./adbfsm mount
[adbfsm] checking adb availability...
[adbfsm] multiple devices detected,
         - 1: 068832516O101622
         - 2: 192.168.240.112:5555
[adbfsm] please specify which one you would like to use: _
```

`adbfsm` respects the env variable `ANDROID_SERIAL` (mimicking `adb` behavior) so you can alternately use it to specify the device.

```sh
$ ANDROID_SERIAL=068832516O101622 ./adbfsm
[adbfsm] checking adb availability...
[adbfsm] using serial '068832516O101622' from env variable 'ANDROID_SERIAL'

```

### Cache size

`adbfsm` caches all the read/write operations on the files on the device. This cache is stored in memory. You can control the size of this cache using `--cache-size` option (in MiB). The default value is `512` (512MiB).

```sh
$ ./adbfsm --cache-size=512<mountpoint>    # using 512MiB of cache
```

### Page size

In the cache, each file is divided into pages. The `--page-size` option dictates the size of this page (in KiB). Page size also dictates the size of the buffer used to read/write into the file on the device. You can adjust this value according to your use. From my testing, `page-size` of value `128` (means 128KiB) works well when using USB cable for the `adb` connection. You may want to decrease or increase this value for your use case. The default value is `128` (128KiB).

```sh
$ ./adbfsm --page-size=128<mountpoint>    # using 128KiB of page size

```

### Logging

The default log file is stdout (which goes to nowhere when not run in foreground mode). You can manually set the log file using `--log-file` option and set the log level using `--log-level`.

```sh
$ ./adbfsm --log-file=adbfsm.log --log-level=debug <mountpoint>
```

### Debug mode

As part of debugging functionality `libfuse` has provided debug mode through `-d` flag. You can use this to monitor `adbfsm` operations (if you don't want to use log file or want to see the log in real-time). If the debugging information is too verbose, you can use `-f` instead to make adbfsm run in foreground mode without printing `fuse` debug information.

```sh
$ ./adbfsm --log-file=- --log-level=debug -d <mountpoint>                     # this will print the libfuse debug messages and adbfsm log messages
$ ./adbfsm --log-file=- --log-level=debug -d <mountpoint> 2> /dev/null        # this will print only adbfsm log messages since libfuse debug messages are printed to stderr
```

## TODO

- [ ] Automatic unmount on device disconnect.
- [ ] Periodic cache invalidation. Current implementation only look at the size of current cache and only invalidate oldest entry when newest entry is added and the size exceed the `cache-size` limit.
- [x] IPC to talk to the `adbfsm` to control the filesystem parameters like invalidation, timeout, cache size, etc.
- [x] Implement the filesystem as actual tree for caching the stat.
- [x] Implement file read and write operation caching in memory.
- [ ] Implement versioning on each node that expires every certain period of time. When a node expires it needs to query the files from the device again.
- [x] Implement proper multithreading. Current implementation is practically single threaded (the tree is locked every time it is used) which is not ideal.
