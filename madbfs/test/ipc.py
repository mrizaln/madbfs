#!/usr/bin/env python

import json
import struct
import sys
import time
from argparse import ArgumentParser
from socket import AF_UNIX, SOCK_STREAM, socket


class Protocol:
    @staticmethod
    def receive(sock: socket) -> tuple[str, int] | None:
        (raw_msglen, packet_count) = Protocol.__recvall(sock, 4)
        if len(raw_msglen) <= 0:
            return None

        msglen = struct.unpack(">I", raw_msglen)[0]
        (data, packet_count) = Protocol.__recvall(sock, msglen)
        if data is None or len(data) != msglen:
            return None

        return data.decode(), packet_count

    @staticmethod
    def send(sock: socket, string: str) -> bool:
        msglen = struct.pack(">I", len(string))
        try:
            sock.sendall(msglen)
            sock.sendall(string.encode())
        except Exception as e:
            eprint(e)
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


def try_connect(serial: str, retry_attempt: int, retry_delay: int) -> socket | None:
    sock = socket(AF_UNIX, SOCK_STREAM)

    retry_atttempt = 0
    while True:
        try:
            sock.connect(f"/run/user/1000/madbfs@{serial}.sock")
            return sock
        except ConnectionRefusedError:
            eprint(
                f"Failed to connect. Retrying in {retry_delay} second. Attempt {retry_atttempt} / {retry_attempt}"
            )
            time.sleep(retry_delay)
            retry_atttempt += 1

            if retry_atttempt > retry_attempt:
                return None


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def main() -> int:
    parser = ArgumentParser(
        description="Simple TCP client with length-based message protocol"
    )

    parser.add_argument("serial", type=str, help="adb serial")
    parser.add_argument(
        "--retry-attempt",
        type=int,
        default=5,
        help="number of retry attempts before giving up",
    )
    parser.add_argument(
        "--retry-delay",
        type=int,
        default=1,
        help="delay in seconds before retrying",
    )
    parser.add_argument(
        "--verbose", action="store_true", help="print debug information"
    )

    args = parser.parse_args()

    sock = try_connect(args.serial, args.retry_attempt, args.retry_delay)
    if sock is None:
        eprint("Failed to connect to server. Exiting...")
        return 1

    # op = {"op": "get_cache_size"}
    # op = {"op": "set_cache_size", "value": {"mib": 128}}

    # op = {"op": "get_page_size"}
    # op = {"op": "set_page_size", "value": {"kib": 128}}
    #
    op = {"op": "invalidate_cache"}
    # op = {"op": "help"}

    Protocol.send(sock, json.dumps(op))
    resp = Protocol.receive(sock)
    if resp is not None:
        print(resp[0])

    eprint("closing socket")
    sock.close()

    return 0


if __name__ == "__main__":
    ret = main()
    exit(ret)
