#!/usr/bin/env python3
"""PC-side validation for candidate Lepton RAW16 lossless frame packing.

This script does not talk to hardware and does not change the UART protocol.
It verifies that the proposed row12-mix payload can round-trip synthetic
160x120 RAW16 thermal frames before the STM32 firmware only estimates its size.
"""

from __future__ import annotations

import random
import sys
import time
from dataclasses import dataclass
from typing import Callable


WIDTH = 160
HEIGHT = 120
RAW_LEN = WIDTH * HEIGHT * 2
ROW_FLAG_LEN = (HEIGHT + 7) // 8


Frame = list[list[int]]


@dataclass(frozen=True)
class CaseResult:
    name: str
    encoded_len: int
    packed_rows: int
    raw_rows: int
    ratio: float
    encode_us: float
    decode_us: float


def _u16be(v: int) -> bytes:
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


def encode_row12_mix(frame: Frame) -> tuple[bytes, int, int]:
    flags = bytearray(ROW_FLAG_LEN)
    out = bytearray(ROW_FLAG_LEN)
    packed_rows = 0
    raw_rows = 0

    for y, row in enumerate(frame):
        row_min = min(row)
        row_max = max(row)

        if row_max - row_min <= 0x0FFF:
            flags[y >> 3] |= 1 << (y & 7)
            packed_rows += 1
            out += _u16be(row_min)
            for x in range(0, WIDTH, 2):
                d0 = row[x] - row_min
                d1 = row[x + 1] - row_min
                out.append((d0 >> 4) & 0xFF)
                out.append(((d0 & 0x0F) << 4) | ((d1 >> 8) & 0x0F))
                out.append(d1 & 0xFF)
        else:
            raw_rows += 1
            for v in row:
                out += _u16be(v)

    out[:ROW_FLAG_LEN] = flags
    return bytes(out), packed_rows, raw_rows


def decode_row12_mix(payload: bytes) -> Frame:
    if len(payload) < ROW_FLAG_LEN:
        raise ValueError("payload too short")

    pos = ROW_FLAG_LEN
    frame: Frame = []
    for y in range(HEIGHT):
        packed = (payload[y >> 3] >> (y & 7)) & 1
        row: list[int] = []

        if packed:
            if pos + 2 + WIDTH * 12 // 8 > len(payload):
                raise ValueError("truncated packed row")
            base = (payload[pos] << 8) | payload[pos + 1]
            pos += 2
            for _ in range(WIDTH // 2):
                b0 = payload[pos]
                b1 = payload[pos + 1]
                b2 = payload[pos + 2]
                pos += 3
                d0 = (b0 << 4) | (b1 >> 4)
                d1 = ((b1 & 0x0F) << 8) | b2
                row.append(base + d0)
                row.append(base + d1)
        else:
            if pos + WIDTH * 2 > len(payload):
                raise ValueError("truncated raw row")
            for _ in range(WIDTH):
                row.append((payload[pos] << 8) | payload[pos + 1])
                pos += 2

        frame.append(row)

    if pos != len(payload):
        raise ValueError(f"trailing bytes: {len(payload) - pos}")
    return frame


def estimate_delta8_len(frame: Frame) -> int:
    """One possible future variable-length predictor, length only.

    Per row: first pixel raw16, then int8 left-delta if in [-127, 127],
    otherwise escape byte 0x80 + raw16. This is not the Step-1 firmware change;
    it is kept here only to compare whether row12 is worth pursuing first.
    """
    length = 0
    for row in frame:
        length += 2
        prev = row[0]
        for v in row[1:]:
            d = v - prev
            length += 1 if -127 <= d <= 127 else 3
            prev = v
    return length


def frame_flat() -> Frame:
    return [[30000 for _ in range(WIDTH)] for _ in range(HEIGHT)]


def frame_smooth() -> Frame:
    return [
        [29500 + x * 3 + y * 2 + ((x * y) % 17) for x in range(WIDTH)]
        for y in range(HEIGHT)
    ]


def frame_hotspot() -> Frame:
    frame = frame_smooth()
    cx, cy = 80, 60
    for y in range(HEIGHT):
        for x in range(WIDTH):
            dx = x - cx
            dy = y - cy
            r2 = dx * dx + dy * dy
            if r2 < 30 * 30:
                frame[y][x] += max(0, 1800 - r2 * 2)
    return frame


def frame_mixed_rows() -> Frame:
    frame = frame_smooth()
    for y in range(0, HEIGHT, 10):
        for x in range(WIDTH):
            frame[y][x] = 18000 + x * 180
    return frame


def frame_wide_span() -> Frame:
    return [
        [20000 + x * 100 + y for x in range(WIDTH)]
        for y in range(HEIGHT)
    ]


def frame_boundary_4095() -> Frame:
    return [
        [31000 + ((x * 4095) // (WIDTH - 1)) for x in range(WIDTH)]
        for _ in range(HEIGHT)
    ]


def frame_boundary_4096() -> Frame:
    return [
        [31000 + ((x * 4096) // (WIDTH - 1)) for x in range(WIDTH)]
        for _ in range(HEIGHT)
    ]


def frame_random(seed: int = 20260707) -> Frame:
    rng = random.Random(seed)
    return [[rng.randrange(0, 65536) for _ in range(WIDTH)] for _ in range(HEIGHT)]


def run_case(name: str, factory: Callable[[], Frame]) -> CaseResult:
    frame = factory()
    t0 = time.perf_counter_ns()
    payload, packed_rows, raw_rows = encode_row12_mix(frame)
    t1 = time.perf_counter_ns()
    decoded = decode_row12_mix(payload)
    t2 = time.perf_counter_ns()
    if decoded != frame:
        raise AssertionError(f"{name}: decoded frame differs")

    return CaseResult(
        name=name,
        encoded_len=len(payload),
        packed_rows=packed_rows,
        raw_rows=raw_rows,
        ratio=len(payload) / RAW_LEN,
        encode_us=(t1 - t0) / 1000.0,
        decode_us=(t2 - t1) / 1000.0,
    )


def main() -> int:
    cases: list[tuple[str, Callable[[], Frame]]] = [
        ("flat", frame_flat),
        ("smooth", frame_smooth),
        ("hotspot", frame_hotspot),
        ("mixed_rows", frame_mixed_rows),
        ("wide_span", frame_wide_span),
        ("boundary_4095", frame_boundary_4095),
        ("boundary_4096", frame_boundary_4096),
        ("random_noise", frame_random),
    ]

    print("row12_mix round-trip validation")
    print(f"frame={WIDTH}x{HEIGHT} raw={RAW_LEN}B flags={ROW_FLAG_LEN}B")
    print("case              len    ratio   packed raw  enc_us  dec_us  delta8_len")
    print("---------------- ------ ------- ------ --- ------- ------- ----------")
    for name, factory in cases:
        result = run_case(name, factory)
        delta8_len = estimate_delta8_len(factory())
        print(
            f"{result.name:<16} {result.encoded_len:6d} "
            f"{result.ratio:7.3f} {result.packed_rows:6d} {result.raw_rows:3d} "
            f"{result.encode_us:7.1f} {result.decode_us:7.1f} {delta8_len:10d}"
        )

    print("PASS: row12_mix decode output exactly matches every input frame")
    return 0


if __name__ == "__main__":
    sys.exit(main())
