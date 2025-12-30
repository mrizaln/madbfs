#!/usr/bin/env python3

"""
this test file is adapted from example filesystem test script in libfuse repository:
    https://github.com/libfuse/libfuse/blob/b773020464641d3e9cec5ad5fa35e7153e54e118/test/test_examples.py
"""

if __name__ == "__main__":
    import sys

    import pytest

    sys.exit(pytest.main([__file__] + sys.argv[1:]))

import errno
import filecmp
import json
import logging
import os
import re
import shutil
import stat
import struct
import sys
import time
from contextlib import contextmanager
from dataclasses import astuple, dataclass
from pathlib import Path
from socket import AF_UNIX, SOCK_STREAM, socket
from subprocess import (
    DEVNULL,
    PIPE,
    STDOUT,
    CalledProcessError,
    Popen,
    TimeoutExpired,
    run,
)
from tempfile import NamedTemporaryFile

import pytest

CURRENT_DIR = Path(os.path.dirname(__file__))
TEST_FILE = CURRENT_DIR / "test_file.txt"
TEST_DATA = bytearray()
PROJECT_ROOT = CURRENT_DIR / ".."
BINARY_PATH = PROJECT_ROOT / "build/Release/madbfs/madbfs"
SERVER_PATH = PROJECT_ROOT / "madbfs-server/build/android-all-release/madbfs-server"

DEFAULT_PAGE_SIZE = 128  # in KiB
DEFAULT_CACHE_SIZE = 256  # in MiB
DEFAULT_TTL = 30  # in seconds
DEFAULT_TIMEOUT = 10  # in seconds
DEFAULT_LOG_LEVEL = "debug"  # on this test only

logger = logging.getLogger(__name__)


@dataclass
class Environ:
    serial: str
    abi: str
    mount_point: Path
    test_dir: Path
    log_path: Path
    mount_cmd: list[str]
    server: bool


class Protocol:
    @staticmethod
    def receive(sock: socket) -> str | None:
        (raw_msglen, _) = Protocol.__recvall(sock, 4)
        if len(raw_msglen) <= 0:
            return None

        msglen = struct.unpack(">I", raw_msglen)[0]
        (data, _) = Protocol.__recvall(sock, msglen)
        if data is None or len(data) != msglen:
            return None

        return data.decode()

    @staticmethod
    def send(sock: socket, string: str) -> bool:
        msglen = struct.pack(">I", len(string))
        try:
            sock.sendall(msglen)
            sock.sendall(string.encode())
        except Exception as e:
            logger.warning(e)
            return False
        return True

    @staticmethod
    def __recvall(sock: socket, n: int) -> tuple[bytearray, int]:
        # Helper function to recv n bytes or return None if EOF is hit
        data = bytearray()
        packet_count = 0
        while len(data) < n:
            packet = sock.recv(n - len(data))
            if not packet:
                break
            data.extend(packet)
            packet_count += 1
        return data, packet_count


@pytest.fixture(params=[True, False])
# @pytest.fixture(params=[True])
def environ(request):
    serial = os.environ.get("ANDROID_SERIAL")
    if serial is None:
        pytest.fail("test requires ANDROID_SERIAL environment variable to be set")

    if not BINARY_PATH.exists():
        pytest.fail(f"binary path '{BINARY_PATH}' doesn't exists. compile it first!")

    cmd = ["adb", "shell", "getprop", "ro.product.cpu.abi"]
    abi = run(cmd, check=True, stdout=PIPE)
    abi = abi.stdout.decode("utf-8").strip()

    server_path = SERVER_PATH.parent / f"{SERVER_PATH.name}-{abi}"
    if not server_path.exists():
        pytest.fail(f"server path '{server_path}' doesn't exists. compile it first!")

    mount_point = CURRENT_DIR / "mount"

    if not mount_point.exists():
        mount_point.mkdir(parents=True)
    elif not mount_point.is_dir():
        pytest.fail(f"mount point {mount_point} is not a directory")
    elif len(os.listdir(mount_point)) > 0:
        pytest.fail(f"mount point {mount_point} is not empty")

    test_dir = mount_point / "data/local/tmp"  # testing on /sdcard is risky...
    log_path = CURRENT_DIR / "test.log"

    mount_cmd = [
        BINARY_PATH,
        "-f",
        f"--log-file={log_path}",
        f"--log-level={DEFAULT_LOG_LEVEL}",
        f"--server={server_path}" if request.param else "--no-server",
    ]

    return Environ(
        abi=abi,
        serial=serial,
        mount_point=mount_point,
        test_dir=test_dir,
        log_path=log_path,
        mount_cmd=mount_cmd,
        server=request.param,
    )


def ipc_connect(serial: str) -> socket:
    sock = socket(AF_UNIX, SOCK_STREAM)

    max = 5
    tries = 1

    while tries <= max:
        try:
            path = os.environ["XDG_RUNTIME_DIR"]
            sock.connect(f"{path}/madbfs@{serial}.sock")
            return sock
        except ConnectionRefusedError:
            logger.warning(f"Failed to connect. Retry in 1 seconds ({tries} / {max})")
            time.sleep(1)
            tries += 1

    pytest.fail("failed to connect to socket")


def wait_for_mount(mount_process: Popen[str], mnt_dir: Path):
    elapsed = 0
    while elapsed < 30:
        if os.path.ismount(mnt_dir):
            return True
        if mount_process.poll() is not None:
            pytest.fail("file system process terminated prematurely")
        time.sleep(0.1)
        elapsed += 0.1
    pytest.fail("mountpoint failed to come up")


def unmount(mount_process: Popen[str], mnt_dir: Path):
    cmd = ["fusermount3", "-z", "-u", mnt_dir]
    try:
        result = run(cmd, capture_output=True, text=True, check=True)
        if result.stdout:
            logger.debug(f"Unmount command stdout: {result.stdout}")
        if result.stderr:
            logger.debug(f"Unmount command stderr: {result.stderr}")
    except CalledProcessError as e:
        logger.error(
            f"Unmount command failed with return code {e.returncode}\nStdout: {e.stdout}\nStderr: {e.stderr}"
        )
        raise

    if not os.path.ismount(mnt_dir):
        logger.info(f"{mnt_dir} is no longer a mount point")
    else:
        logger.warning(f"{mnt_dir} is still a mount point after unmount command")

    # Give mount process a little while to terminate. Popen.wait(timeout)
    # was only added in 3.3...
    elapsed = 0
    while elapsed < 30:
        code = mount_process.poll()
        if code is not None:
            if code == 0:
                return
            logger.error(f"File system process terminated with code {code}")
            pytest.fail(f"file system process terminated with code {code}")
        time.sleep(0.1)
        elapsed += 0.1
    logger.error("Mount process did not terminate within 30 seconds")
    pytest.fail("mount process did not terminate")


def cleanup(mount_process: Popen[str], mnt_dir: Path):
    cmd = ["fusermount3", "-z", "-u", mnt_dir]
    run(cmd, stdout=DEVNULL, stderr=STDOUT)

    mount_process.terminate()
    try:
        mount_process.wait(5)
    except TimeoutExpired:
        mount_process.kill()

    # TODO: adb child process cleanup as well
    # TODO: ipc cleanup


def name_generator(__ctr=[0]):
    __ctr[0] += 1
    return "testfile_%d" % __ctr[0]


def os_create(name: Path):
    os.close(os.open(name, os.O_CREAT | os.O_RDWR))


@contextmanager
def os_open(name, flags):
    fd = os.open(name, flags)
    try:
        yield fd
    finally:
        os.close(fd)


def tst_readdir(work_dir: Path):
    newdir = name_generator()

    mnt_newdir = work_dir / newdir

    file = mnt_newdir / name_generator()
    subdir = mnt_newdir / name_generator()
    subfile = subdir / name_generator()

    mnt_newdir.mkdir()
    shutil.copyfile(TEST_FILE, file)
    subdir.mkdir()
    shutil.copyfile(TEST_FILE, subfile)

    listdir_is = os.listdir(mnt_newdir)
    listdir_is.sort()
    listdir_should = [os.path.basename(file), os.path.basename(subdir)]
    listdir_should.sort()

    assert listdir_is == listdir_should

    file.unlink()
    subfile.unlink()
    subdir.rmdir()
    mnt_newdir.rmdir()


def tst_readdir_big(work_dir: Path):
    # Add enough entries so that readdir needs to be called multiple times.
    files = []

    for i in range(500):
        prefix = "A rather long filename to make sure that we fill up the buffer - "
        file = work_dir / f"{prefix * 3}{i}"
        with open(file, "w") as fh:
            fh.write("File %d" % i)

        files.append(file)

    listdir_is = sorted(os.listdir(work_dir))
    listdir_should = sorted(file.name for file in files)

    assert listdir_is == listdir_should

    for file in files:
        file.unlink()


def tst_open_read(work_dir: Path):
    file = work_dir / name_generator()

    with open(file, "wb") as fh_out, open(TEST_FILE, "rb") as fh_in:
        shutil.copyfileobj(fh_in, fh_out)

    assert filecmp.cmp(file, TEST_FILE, False)

    file.unlink()


def tst_open_write(work_dir: Path):
    file = work_dir / name_generator()

    os_create(file)
    with open(file, "wb") as fh_out, open(TEST_FILE, "rb") as fh_in:
        shutil.copyfileobj(fh_in, fh_out)

    assert filecmp.cmp(file, TEST_FILE, False)

    file.unlink()


def tst_open_write_big(work_dir: Path):
    file = work_dir / name_generator()

    os_create(file)
    test_data_big = bytearray()

    target_size = DEFAULT_PAGE_SIZE * 1024 * 512

    with open(__file__, "rb") as fh:
        buf = fh.read()
        while len(test_data_big) < target_size:
            test_data_big.extend(buf)

    logger.info(f"write {len(test_data_big)} bytes into {file}")

    with open(file, "wb") as fh:
        fh.write(test_data_big)

    with open(file, "rb") as fh:
        assert fh.read() == test_data_big

    file.unlink()


def tst_create(work_dir: Path):
    file = work_dir / name_generator()

    with pytest.raises(OSError) as exc_info:
        file.stat()
    assert exc_info.value.errno == errno.ENOENT
    assert file.name not in os.listdir(work_dir)

    fd = os.open(file, os.O_CREAT | os.O_RDWR)
    os.close(fd)

    assert file.name in os.listdir(work_dir)
    fstat = file.lstat()
    assert stat.S_ISREG(fstat.st_mode)
    assert fstat.st_nlink == 1
    assert fstat.st_size == 0

    file.unlink()


def tst_append(work_dir: Path):
    file = work_dir / name_generator()

    os_create(file)
    with os_open(file, os.O_WRONLY) as fd:
        os.write(fd, b"foo\n")
    with os_open(file, os.O_WRONLY | os.O_APPEND) as fd:
        os.write(fd, b"bar\n")

    with open(file, "rb") as fh:
        assert fh.read() == b"foo\nbar\n"

    file.unlink()


def tst_seek(work_dir: Path):
    file = work_dir / name_generator()

    os_create(file)
    with os_open(file, os.O_WRONLY) as fd:
        os.lseek(fd, 1, os.SEEK_SET)
        os.write(fd, b"foobar\n")
    with os_open(file, os.O_WRONLY) as fd:
        os.lseek(fd, 4, os.SEEK_SET)
        os.write(fd, b"com")

    with open(file, "rb") as fh:
        assert fh.read() == b"\0foocom\n"

    file.unlink()


def tst_mkdir(work_dir: Path):
    dir = work_dir / name_generator()

    dir.mkdir()
    fstat = dir.stat()
    assert stat.S_ISDIR(fstat.st_mode)
    assert os.listdir(dir) == []
    # Some filesystem (e.g. BTRFS) don't track st_nlink for directories
    assert fstat.st_nlink in (1, 2)
    assert dir.name in os.listdir(work_dir)

    dir.rmdir()


def tst_rmdir(work_dir: Path):
    dir = work_dir / name_generator()

    dir.mkdir()
    assert dir.name in os.listdir(work_dir)
    dir.rmdir()
    with pytest.raises(OSError) as exc_info:
        dir.stat()
    assert exc_info.value.errno == errno.ENOENT
    assert dir.name not in os.listdir(work_dir)


def tst_unlink(work_dir: Path):
    file = work_dir / name_generator()
    with open(file, "wb") as fh:
        fh.write(b"hello")
    assert file.name in os.listdir(work_dir)
    file.unlink()
    with pytest.raises(OSError) as exc_info:
        file.stat()
    assert exc_info.value.errno == errno.ENOENT
    assert file.name not in os.listdir(work_dir)


def tst_symlink(work_dir: Path):
    link = work_dir / name_generator()
    with pytest.raises(OSError) as exc_info:
        link.symlink_to("/imaginary/dest")
    assert exc_info.value.errno == errno.ENOSYS
    assert link.name not in os.listdir(work_dir)


def tst_chown(work_dir: Path):
    dir = work_dir / name_generator()

    dir.mkdir()
    fstat = dir.lstat()
    uid = fstat.st_uid
    gid = fstat.st_gid

    uid_new = uid + 1
    with pytest.raises(OSError) as exc_info:
        os.chown(dir, uid_new, -1)
    assert exc_info.value.errno == errno.ENOSYS

    gid_new = gid + 1
    with pytest.raises(OSError) as exc_info:
        os.chown(dir, gid_new, -1)
    assert exc_info.value.errno == errno.ENOSYS

    dir.rmdir()


# Underlying fs may not have ns resolution
def tst_utimens(work_dir: Path, ns_tol=1000):
    dir = work_dir / name_generator()

    dir.mkdir()
    fstat = dir.lstat()

    logger.debug(
        f" sec=[a:{fstat.st_atime}, m:{fstat.st_mtime}] nsec=[a:{fstat.st_atime_ns}, m:{fstat.st_mtime_ns}]"
    )

    atime = fstat.st_atime + 42.28
    mtime = fstat.st_mtime - 42.23
    if sys.version_info < (3, 3):
        os.utime(dir, (atime, mtime))
    else:
        atime_ns = fstat.st_atime_ns + int(42.28 * 1e9)
        mtime_ns = fstat.st_mtime_ns - int(42.23 * 1e9)
        os.utime(dir, None, ns=(atime_ns, mtime_ns))

    logger.debug(f" sec=[a:{atime}, m:{mtime}] nsec=[a:{atime_ns}, m:{mtime_ns}]")

    fstat = dir.lstat()

    logger.debug(
        f" sec=[a:{fstat.st_atime}, m:{fstat.st_mtime}] nsec=[a:{fstat.st_atime_ns}, m:{fstat.st_mtime_ns}]"
    )

    assert abs(fstat.st_atime - atime) < 1
    assert abs(fstat.st_mtime - mtime) < 1
    if sys.version_info >= (3, 3):
        assert abs(fstat.st_atime_ns - atime_ns) <= ns_tol
        assert abs(fstat.st_mtime_ns - mtime_ns) <= ns_tol

    dir.rmdir()


def tst_link(work_dir: Path):
    name1 = work_dir / name_generator()
    name2 = work_dir / name_generator()

    shutil.copyfile(TEST_FILE, name1)
    assert filecmp.cmp(name1, TEST_FILE, False)

    fstat1 = os.lstat(name1)
    assert fstat1.st_nlink == 1

    with pytest.raises(OSError) as exc_info:
        os.link(name1, name2)
    assert exc_info.value.errno == errno.EPERM

    name1.unlink()


def tst_truncate_path(work_dir: Path):
    assert len(TEST_DATA) > 1024

    file = work_dir / name_generator()
    with file.open("wb") as fh:
        fh.write(TEST_DATA)

    fstat = os.stat(file)
    size = fstat.st_size
    assert size == len(TEST_DATA)

    # Add zeros at the end
    os.truncate(file, size + 1024)
    assert file.stat().st_size == size + 1024
    with file.open("rb") as fh:
        assert fh.read(size) == TEST_DATA
        assert fh.read(1025) == b"\0" * 1024

    # Truncate data
    os.truncate(file, size - 1024)
    assert file.stat().st_size == size - 1024
    with file.open("rb") as fh:
        assert fh.read(size) == TEST_DATA[: size - 1024]

    file.unlink()


def tst_truncate_fd(work_dir: Path):
    assert len(TEST_DATA) > 1024

    with NamedTemporaryFile("w+b", 0, dir=work_dir) as fh:
        fd = fh.fileno()
        fh.write(TEST_DATA)
        fstat = os.fstat(fd)
        size = fstat.st_size
        assert size == len(TEST_DATA)

        # Add zeros at the end
        os.ftruncate(fd, size + 1024)
        assert os.fstat(fd).st_size == size + 1024
        fh.seek(0)
        assert fh.read(size) == TEST_DATA
        assert fh.read(1025) == b"\0" * 1024

        # Truncate data
        os.ftruncate(fd, size - 1024)
        assert os.fstat(fd).st_size == size - 1024
        fh.seek(0)
        assert fh.read(size) == TEST_DATA[: size - 1024]


def tst_open_unlink(work_dir: Path):
    file = work_dir / name_generator()

    data1 = b"foo"
    data2 = b"bar"
    with open(file, "wb+", buffering=0) as fh:
        fh.write(data1)
        file.unlink()
        with pytest.raises(OSError) as exc_info:
            file.stat()
        assert exc_info.value.errno == errno.ENOENT
        assert file.name not in os.listdir(work_dir)
        fh.write(data2)
        fh.seek(0)
        assert fh.read() == data1 + data2


def tst_open_rename(work_dir: Path):
    file = work_dir / name_generator()

    file2 = work_dir / name_generator()

    data1 = b"foo"
    data2 = b"bar"
    with open(file, "wb+", buffering=0) as fh:
        fh.write(data1)
        file.rename(file2)
        with pytest.raises(OSError) as exc_info:
            file.stat()
        assert exc_info.value.errno == errno.ENOENT
        assert file.name not in os.listdir(work_dir)
        fh.write(data2)
        fh.seek(0)
        assert file.name not in os.listdir(work_dir)
        assert fh.read() == data1 + data2

    file2.unlink()


# first args is there just for symmetry, it's unused
def tst_ipc(work_dir: str, serial: str, server_used: bool):
    version_re = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
    connection = "server" if server_used else "adb"
    timeout = DEFAULT_TIMEOUT if server_used else 0

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "help"}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "version"}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert isinstance(resp["value"]["version"], str)
        assert version_re.match(resp["value"]["version"])

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "info"}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["connection"] == connection
        assert resp["value"]["log_level"] == DEFAULT_LOG_LEVEL
        assert resp["value"]["ttl"] == DEFAULT_TTL
        assert resp["value"]["timeout"] == timeout
        assert resp["value"]["page_size"] == DEFAULT_PAGE_SIZE
        assert resp["value"]["cache_size"]
        assert resp["value"]["cache_size"]["max"] == DEFAULT_CACHE_SIZE
        assert isinstance(resp["value"]["cache_size"]["current"], int)

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "invalidate_cache"}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert isinstance(resp["value"]["size"], int)

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "set_page_size", "value": 256}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["page_size"]
        assert resp["value"]["page_size"]["old"] == DEFAULT_PAGE_SIZE
        assert resp["value"]["page_size"]["new"] == 256
        assert resp["value"]["cache_size"]
        assert isinstance(resp["value"]["cache_size"]["old"], int)
        assert isinstance(resp["value"]["cache_size"]["new"], int)

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "set_cache_size", "value": 128}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["cache_size"]
        assert resp["value"]["cache_size"]["old"] == DEFAULT_CACHE_SIZE
        assert resp["value"]["cache_size"]["new"] == 128

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "set_ttl", "value": 20}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["ttl"]
        assert resp["value"]["ttl"]["old"] == DEFAULT_TTL
        assert resp["value"]["ttl"]["new"] == 20

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "set_timeout", "value": 5}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["timeout"]
        assert resp["value"]["timeout"]["old"] == timeout
        assert resp["value"]["timeout"]["new"] == 5

    with ipc_connect(serial) as sock:
        Protocol.send(sock, json.dumps({"op": "set_log_level", "value": "info"}))
        resp = Protocol.receive(sock)
        assert resp is not None
        logger.info(resp)
        resp = json.loads(resp)
        assert resp["status"] == "success"
        assert resp["value"]
        assert resp["value"]["log_level"]
        assert resp["value"]["log_level"]["old"] == DEFAULT_LOG_LEVEL
        assert resp["value"]["log_level"]["new"] == "info"

    # restore immediately since I need the logs to still be in debug :P
    with ipc_connect(serial) as sock:
        Protocol.send(
            sock, json.dumps({"op": "set_log_level", "value": DEFAULT_LOG_LEVEL})
        )
        resp = Protocol.receive(sock)
        assert resp is not None


def test_filesystem(environ):
    serial: str
    abi: str
    mount_point: Path
    test_dir: Path
    log_file: Path
    cmd_base: list[str]
    server: bool

    serial, abi, mount_point, test_dir, log_file, cmd_base, server = astuple(environ)

    logger.info(f"test is [running serial={serial}, abi={abi}, server={server}]")

    cmd = cmd_base + [f"--serial={serial}", str(mount_point)]
    proc = Popen(cmd, stdout=PIPE, universal_newlines=True)

    global TEST_DATA
    global TEST_FILE

    # generate at least 3 pages worth of data
    with open(__file__, "rb") as fh:
        test_file_data = fh.read()
        while len(TEST_DATA) < 3 * DEFAULT_PAGE_SIZE * 1024:
            TEST_DATA.extend(test_file_data)
        TEST_FILE.write_bytes(TEST_DATA)

    try:
        wait_for_mount(proc, mount_point)
        logger.info(f"filesystem is mounted at {mount_point}")

        work_dir = test_dir / "testing"
        if work_dir.exists():
            shutil.rmtree(work_dir)
        work_dir.mkdir(exist_ok=True)

        def call(fn, *args):
            logger.info(f"testing: {fn.__name__}", stacklevel=2)
            fn(work_dir, *args)

        call(tst_readdir)
        call(tst_readdir_big)
        call(tst_open_read)
        call(tst_open_write)
        call(tst_open_write_big)
        call(tst_create)
        call(tst_append)
        call(tst_seek)
        call(tst_mkdir)
        call(tst_rmdir)
        call(tst_unlink)
        call(tst_symlink)
        call(tst_chown)
        call(tst_utimens, 1000 if server else 1000000000)  # tolerance in nanoseconds
        call(tst_link)
        call(tst_truncate_path)
        call(tst_truncate_fd)
        call(tst_open_unlink)
        call(tst_open_rename)
        call(tst_ipc, serial, server)
    except:
        # NOTE: if tests are failing, the work_dir might not be cleaned up correctly. I
        # won't clean them up here since I might want to inspect the file in the work_dir
        # for debugging purposes.

        cleanup(proc, mount_point)
        raise
    else:
        unmount(proc, mount_point)
    finally:
        TEST_DATA.clear()
        TEST_FILE.unlink()
