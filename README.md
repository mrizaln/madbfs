# adbfsm

The name of the project can be separated as `adb`, `fs`, and `m` which means `adb filesystem modern`.

This project aims to create a well-built filesystem abstraction over `adb` using `libfuse` that is fast, safe, and reliable while also have have well structured code according to modern C++ (20 and above) practices.

## Why?

I want to manage my Android phone storage from my computer without using the terrible MTP.

This project is primarily inspired by the [`adbfs-rootless`](https://github.com/spion/adbfs-rootless) project by Spion, available on GitHub. While his work is highly commendable and greatly appreciated, my experience with it has been marked by frequent crashes. Although I considered updating the code, I found that the codebase was somewhat outdated. As a result, I decided to start from scratch in order to develop a more stable and modern solution.

## Dependencies

Build dependencies

- CMake
- Conan

Library dependencies

- libfuse
- spdlog
- fmt
- subprocess (FetchContent)

> The dependencies are all managed by Conan except when specified

## Building

Since the dependencies are all managed by Conan, you need to make sure it's installed and configured correctly (consult the documentation on how to do it, [here](https://docs.conan.io/2/installation.html)).

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
    --serial=<s>        serial number of the device to mount
                        (default: <auto> [detection is similar to adb])
    --loglevel=<l>      log level to use (default: warn)
    --logfile=<f>       log file to write to (default: - for stdout)
    --cachesize=<n>     maximum size of the cache in MB (default: 500)
    -h, --help          show this help message
    --full-help         show full help message (includes libfuse options)
```

### Selecting device

To mount your device you just need to specify the mount point if there is only one device. If there are more than one device then you can specify the serial using `--serial` option. If you omit the `--serial` option when there are multiple device connected to the computer, you will be prompted to specify the device you want to mount.

```sh
$ ./adbfsm mount
[adbfsm] checking adb availability...
[adbfsm] multiple devices detected,
         - 1: 068832516O101622
         - 2: 192.168.240.112:5555
[adbfsm] please specify which one you would like to use: _
```

`adbfsm` respects the env variable `ANDROID_SERIAL` just like `adb` so you can use that instead to specify the device.

```sh
$ ANDROID_SERIAL=068832516O101622 ./adbfsm
[adbfsm] checking adb availability...
[adbfsm] using serial '068832516O101622' from env variable 'ANDROID_SERIAL'

```

### Cache size

`adbfsm` use the `/tmp/` directory for caching the files pulled from the phone. This cache can be large so you can limit or increase the size of this cache. The size of the cache also correlates to the largest file that can be viewed/pulled from the phone so make sure to set it according to your need.

```sh
$ ./adbfsm --cachesize=1000 <mountpoint>    # using 1GB of cache
```

At the moment the cache directory for `adbfsm` can't be changed and the minimum size is `500MB` even when you set it lower it will be set to `500MB`.

If the `/tmp/` directory is using `zram` (`tmpfs`) you might want to be careful to not set the size of the cache too high as it might fill up your RAM quickly

### Logging

The default log file is stdout (which goes to nowhere when not run in debug mode). If you wish, you can set the log file to your own liking using `--logfile` option and set the log level using `--loglevel`.

```sh
$ ./adbfsm --logfile=adbfsm.log --loglevel=debug <mountpoint>
```

### Debug mode

As part of debugging functionality `libfuse` has provided debug mode through `-d` flag. You can use this to monitor `adbfsm` operations (if you don't want to use log file or want to see the log in real-time).

```sh
$ ./adbfsm --logfile=- --loglevel=debug -d <mountpoint>                     # this will print the libfuse debug messages and adbfsm log messages
$ ./adbfsm --logfile=- --loglevel=debug -d <mountpoint> 2> /dev/null        # this will print only adbfsm log messages since libfuse debug messages are printed to stderr
```

## TODO

- [ ] Periodic cache invalidation: current implementation only look at the size of current cache and only invalidate oldest entry when newest entry is added and the size exceed the `cachesize` limit.
- [ ] IPC to talk to the `adbfsm` to control when invalidation happen (force the invalidation to happen for example).
- [ ] Implement the filesystem as actual tree for caching the stat.
