#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 lfdevs
"""Encode synthetic NV12 frames and verify the AVC profile in returned SPS NALs."""

import argparse
import json
import os
import socket
import struct
from pathlib import Path
from fractions import Fraction

from test_decode import HELLO_MAGIC, HELLO_VERSION, resolve_endpoint

PROFILES = {
    "legacy": (5, None),
    "baseline": (8, 66),
    "main": (9, 77),
    "high": (10, 100),
}


def recv_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise RuntimeError("daemon closed the encoder session")
        data.extend(part)
    return bytes(data)


def sps_profile(data):
    for i in range(len(data) - 5):
        if data[i:i + 3] == b"\x00\x00\x01" and data[i + 3] & 31 == 7:
            return data[i + 4], data[i + 5]
    return None


def parse_fps(value):
    try:
        return Fraction(value)
    except (ValueError, ZeroDivisionError) as error:
        raise argparse.ArgumentTypeError("frame rate must be a number or fraction") from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", default=resolve_endpoint())
    parser.add_argument("--profile", choices=PROFILES, default="high")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--bitrate", type=int, default=4_000_000)
    parser.add_argument("--fps", type=parse_fps, default=Fraction(30),
                        help="integer, decimal or fraction, e.g. 30000/1001")
    parser.add_argument("--frames", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if (not 96 <= args.width <= 8192 or not 96 <= args.height <= 4320
            or args.width % 2 or args.height % 2 or args.frames <= 0
            or not Fraction(1, 1000) <= args.fps <= 1000
            or args.fps.numerator > 0xffffffff or args.fps.denominator > 0xffffffff
            or not 1 <= args.bitrate <= 100_000_000):
        parser.error("invalid dimensions, frame count, frame rate or bitrate")

    codec, expected = PROFILES[args.profile]
    packets = []
    observed = None
    endpoint = os.stat(args.endpoint)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(15)
        sock.connect(args.endpoint)
        sock.sendall(struct.pack("!9I", HELLO_MAGIC, HELLO_VERSION, codec,
                                 args.width, args.height, 0,
                                 args.bitrate, args.fps.numerator, args.fps.denominator))
        status, mode, name_len = struct.unpack("!3I", recv_exact(sock, 12))
        if status:
            raise RuntimeError(f"handshake rejected: status={status}, codec={codec}; "
                               "explicit profiles require an updated daemon")
        if name_len & 0x80000000:
            dev_hi, dev_lo, ino_hi, ino_lo = struct.unpack("!4I", recv_exact(sock, 16))
            if ((dev_hi << 32) | dev_lo, (ino_hi << 32) | ino_lo) != (
                    endpoint.st_dev, endpoint.st_ino):
                raise RuntimeError("socket endpoint identity changed")
        name_len &= 0x7fffffff
        if name_len:
            recv_exact(sock, name_len)
        if mode != 0:
            raise RuntimeError("encoder unexpectedly negotiated SHM")

        pixels = args.width * args.height
        for index in range(args.frames):
            frame = bytes([32 + index * 17 % 192]) * pixels + b"\x60\xa0" * (pixels // 4)
            sock.sendall(struct.pack("!I", len(frame)) + frame)
            size, flags, unit = struct.unpack("!3I", recv_exact(sock, 12))
            if not 0 < size <= 64 * 1024 * 1024 or unit != index + 1:
                raise RuntimeError(f"invalid encoder packet: size={size}, unit={unit}")
            if index == 0 and not flags & 1:
                raise RuntimeError("first packet is not a keyframe")
            packet = recv_exact(sock, size)
            profile = sps_profile(packet)
            if profile:
                observed = profile
                if expected is not None and profile[0] != expected:
                    raise RuntimeError(f"requested {expected}, encoded profile_idc={profile[0]}")
                if args.profile == "baseline" and not profile[1] & 0x40:
                    raise RuntimeError("Baseline output lacks the constrained-baseline flag")
            packets.append(packet)
        sock.sendall(struct.pack("!I", 0))

    if observed is None:
        raise RuntimeError("encoder returned no SPS")
    if args.output:
        args.output.write_bytes(b"".join(packets))
    print(json.dumps({"requested": args.profile, "codec": codec,
                      "profile_idc": observed[0], "constraints": observed[1],
                      "fps": str(args.fps), "frames": len(packets),
                      "bytes": sum(map(len, packets))}))


if __name__ == "__main__":
    main()
