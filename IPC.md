## IPC

The communication though the IPC is done using a simple length-value (LV) message protocol. The first 4 bytes of the message is the length of it (excluding itself) in network byte order, and the rest is the payload.

### Request

The payload must be a JSON object in the form that depends on the operation requested. The general form of the JSON is in the form of:

```json
{
  "op": <name>,
  "value": <value>
}
```

Some operations only requires `"op"` field, while some requires `"value"` field. Below is the breakdown:

- `help`

  ```json
  { "op": "help" }
  ```

- `info`

  ```json
  { "op": "info" }
  ```

- `invalidate_cache`

  ```json
  { "op": "invalidate_cache" }
  ```

- `set_page_size`:

  ```json
  { "op": "set_page_size", "value": <uint> }
  ```

  > - unit is in KiB
  > - `uint` must be between 64 and 4096
  > - the value will be rouded up to the nearest multiple of 2

- `set_cache_size`:

  ```json
  { "op": "set_cache_size", "value": <uint> }
  ```

  > - unit is in MiB
  > - `uint` must be greater than or equal to 128
  > - the value will be rounded up to the nearest multiple of 2

- `set_ttl`:

  ```json
  { "op": "set_ttl", "value": <uint> }
  ```

  > - `uint` is in seconds
  > - value of 0 means ttl is disabled (never expires)

- `set_timeout`:

  ```json
  { "op": "set_timeout", "value": <uint> }
  ```

  > - `uint` is in seconds
  > - value of 0 means timeout is disabled

- `set_log_level`

  ```json
  { "op": "set_log_level", "value": <str> }
  ```

  > - `str` must corresponds to log level accepted by the `--log-level` option

- `logcat`

  ```json
  { "op": "logcat", "value": <bool> }
  ```

  > - `bool` is a boolean that indicates whether to send colored logs or not

### Response

The IPC (beside `logcat`) will reply immediately after an operation is completed. The reply is a JSON in the form of:

```json
{
  "status", <"success"|"error">,
  "value": <value>
}
```

The `"value"` field will be filled with the value of the response of the resulting operation. It will contain a string that explains the error if an error status happens.

The `<value>` then will be different depending on the operation performed:

- `info`

  ```json
  {
    "status": "success"
    "value": {
      "connection": <"server"|"adb">,
      "log_level": <str>,
      "ttl": <uint>,
      "timeout": <uint>,
      "page_size": <uint>,
      "cache_size": {
          "max": <uint>,
          "current":  <uint>
      }
    }
  }
  ```

  > - `page_size` unit is in KiB
  > - `cache_size` unit is in MiB
  > - `ttl` unit is in seconds
  > - `timeout` unit is in seconds

- `invalidate_cache`:

  ```json
  {
    "status": "success",
    "value": {
      "size": <uint>
    }
  }
  ```

  > - `size` represent the size of the cache being invalidated
  > - `size` unit is in MiB

- `set_page_size`:

  ```json
  {
    "status": "success",
    "value": {
      "page_size": {
        "old": <uint>,
        "new": <uint>
      },
      "cache_size": {
        "old": <uint>,
        "new": <uint>
      }
    }
  }
  ```

  > - cache size unit is in MiB
  > - page size unit is in KiB

- `set_cache_size`:

  ```json
  {
    "status": "success",
    "value": {
      "cache_size": {
        "old": <uint>,
        "new": <uint>
      }
    }
  }
  ```

  > - unit is in MiB

- `set_ttl`:

  ```json
  {
    "status": "success",
    "value": {
      "ttl": {
        "old": <uint>,
        "new": <uint>
      }
    }
  }
  ```

  > - unit is in seconds

- `set_timeout`:

  ```json
  {
    "status": "success",
    "value": {
      "timeout": {
        "old": <uint>,
        "new": <uint>
      }
    }
  }
  ```

  > - unit is in seconds

- `set_log_level`

  ```json
  {
    "status": "success",
    "value": {
      "log_level": {
        "old": <uint>,
        "new": <uint>
      }
    }
  }
  ```

  > - unit is in seconds

- `logcat`

  Unlike other operations, `logcat` won't response with a JSON immediately on success, but it will response with stream of logs instead. Each entry is encoded with the same LV protocol.

## `madbfs-msg`

```
usage: [options] [message]

options:
  -h [ --help ]                         print help
  -v [ --version ]                      print version
  -c [ --color ] when (=auto)           color the output (only for logcat)
                                        when=[never, always, auto]
  -l [ --list ]                         list mounted devices with active IPC
  -d [ --search-dir ] dir (=/run/user/1000)
                                        specify the search directory for socket
                                        files
  -s [ --serial ] serial (=192.168.240.112:5555)
                                        the serial number of the mounted device
                                        (can be omitted if 'ANDROID_SERIAL' env
                                        is defined)
  --message arg                         message to be passed to madbfs
                                        (positional arguments will be
                                        considered as part of this option)

```

The `message` argument is the desired operation you want to perform through the IPC for the selected `madbfs` instance. The general rule is to transform the operation request JSON into a command. 

For example, to do `set_cache_size` operation, normally you need to send this JSON through IPC with LV encoding:

```json
{ "op": "set_cache_size", "value": 1024 }
```

Using `madbfs-msg` you just need to use it like this:

```sh
madbfs-msg -s 068832516O101622 set_cache_size 1024
```

If the operation JSON doesn't need `value` property, like `info`, you do this:

```sh
madbfs-msg -s 068832516O101622 info
```

For `logcat`, the color is specified using `--color` option.

```sh
madbfs-msg -s 068832516O101622 --color=always logcat
```
