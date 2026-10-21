"""Stream Doom frames to the ESP32-S3 OLED demo and measure the link and per-stage timing.

The laptop runs pipeline stages 1+2 (gray LUT, crop, area downscale) with the same C code as
the ESP32 (via ctypes) and sends 128x64 gray frames using the Phase 1 demo stream protocol
(components/demo_stream/include/demo_stream.h; NOT the brain protocol). The ESP32 dithers,
packs, writes the OLED over I2C, and replies with its timings. Stop-and-wait: one frame in
flight, so the round trip time is the full per-frame cost.

Examples:
    .venv/bin/python host/stream/stream_oled.py --port /dev/ttyACM0 --source live --frames 600
    .venv/bin/python host/stream/stream_oled.py --port /dev/ttyACM0 \
        --source npz --npz host/captures/freedoom1.npz --style fs --fast-i2c
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from oledpipe import OLED_H, OLED_W, OledPipeline, read_playpal  # noqa: E402

FRAME_MAGIC = b"OLF1"
REPLY_MAGIC = b"OLS1"
REPLY_BYTES = 32
REPLY_FMT = (
    "<HBBIIIII"  # seq, status, flags, dither_us, pack_us, i2c_us, crc_err, len_err
)
STYLE_CODES = {"bayer": 0, "fs": 1, "outline": 2}  # DS_STYLE_* in demo_stream.h
FLAG_FAST_I2C = 0x80
STATUS_OK, STATUS_HELLO = 0, 1
STATUS_NAMES = {0: "ok", 1: "hello", 2: "pipeline error", 3: "i2c error"}
REPLY_TIMEOUT_S = 1.0


@dataclass
class Reply:
    seq: int
    status: int
    flags: int
    dither_us: int
    pack_us: int
    i2c_us: int
    crc_errors: int
    len_errors: int


def encode_frame(seq: int, flags: int, gray: np.ndarray) -> bytes:
    payload = np.ascontiguousarray(gray, dtype=np.uint8).tobytes()
    if len(payload) != OLED_W * OLED_H:
        raise ValueError(f"payload {len(payload)} bytes, expected {OLED_W * OLED_H}")
    body = struct.pack("<HBBH", seq & 0xFFFF, flags, 0, len(payload)) + payload
    return FRAME_MAGIC + body + struct.pack("<I", zlib.crc32(body))


class PortBusyError(RuntimeError):
    """The serial port failed in a way that usually means a second program has it open."""


class ReplyReader:
    """Accumulates bytes from the port and extracts CRC-checked replies, skipping boot text."""

    def __init__(self, port: serial.Serial) -> None:
        self._port = port
        self._buf = bytearray()
        self.bad_replies = 0

    def read(self, timeout_s: float) -> Reply | None:
        deadline = time.monotonic() + timeout_s
        while True:
            reply = self._extract()
            if reply is not None:
                return reply
            if time.monotonic() >= deadline:
                return None
            try:
                chunk = self._port.read(max(1, self._port.in_waiting))
            except serial.SerialException as e:
                raise PortBusyError(
                    f"{e}\nIs another program (idf.py monitor?) using the port? Close it first: "
                    f"fuser -v {self._port.port}"
                ) from e
            self._buf.extend(chunk)

    def _extract(self) -> Reply | None:
        while True:
            start = self._buf.find(REPLY_MAGIC)
            if start < 0:
                del self._buf[: max(0, len(self._buf) - (len(REPLY_MAGIC) - 1))]
                return None
            if len(self._buf) - start < REPLY_BYTES:
                del self._buf[:start]
                return None
            raw = bytes(self._buf[start : start + REPLY_BYTES])
            body, crc = raw[4:-4], struct.unpack("<I", raw[-4:])[0]
            if zlib.crc32(body) != crc:
                self.bad_replies += 1
                del self._buf[: start + 1]  # false magic or corruption: resync after it
                continue
            del self._buf[: start + REPLY_BYTES]
            return Reply(*struct.unpack(REPLY_FMT, body))


def open_port(path: str, baud: int) -> serial.Serial:
    """Open without toggling DTR/RTS, which would reset the ESP32-S3 (auto-reset circuit)."""
    port = serial.Serial()
    port.port = path
    port.baudrate = baud  # must match CONFIG_DEMO_UART_BAUD; ignored by USB Serial/JTAG
    port.timeout = 0.05
    port.dtr = False
    port.rts = False
    port.open()
    return port


def npz_frames(path: Path, n: int) -> tuple[np.ndarray, Iterator[np.ndarray]]:
    data = np.load(path)
    frames, palette = data["frames"], data["palette"]

    def gen() -> Iterator[np.ndarray]:
        for i in range(n):
            yield frames[i % len(frames)]

    return palette, gen()


def live_frames(
    scenario: str, n: int, seed: int
) -> tuple[np.ndarray, Iterator[np.ndarray]]:
    import vizdoom as vzd
    from capture_frames import SCENARIOS, make_game

    palette = read_playpal(Path(vzd.__file__).parent / SCENARIOS[scenario])
    rng = np.random.default_rng(seed)

    def gen() -> Iterator[np.ndarray]:
        game = make_game(scenario, seed)
        try:
            turn, left = [0, 0], 0
            for _ in range(n):
                if game.is_episode_finished():
                    game.new_episode()
                if left == 0:
                    turn = [[0, 0], [1, 0], [0, 1]][int(rng.integers(0, 3))]
                    left = int(rng.integers(4, 14))
                left -= 1
                game.make_action([1, turn[0], turn[1], int(rng.random() < 0.1)], 1)
                state = game.get_state()
                if state is not None:
                    yield state.screen_buffer
        finally:
            game.close()

    return palette, gen()


@dataclass
class Stats:
    rtt_ms: list[float] = field(default_factory=list)
    host_ms: list[float] = field(default_factory=list)
    dither_us: list[int] = field(default_factory=list)
    pack_us: list[int] = field(default_factory=list)
    i2c_us: list[int] = field(default_factory=list)
    timeouts: int = 0
    device_errors: int = 0


def summarize(name: str, values: list[float], unit: str) -> str:
    if not values:
        return f"  {name:24s} n/a"
    a = np.asarray(values, dtype=np.float64)
    return (
        f"  {name:24s} mean {a.mean():8.2f} {unit}  p50 {np.percentile(a, 50):8.2f}"
        f"  p95 {np.percentile(a, 95):8.2f}  max {a.max():8.2f}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--port", required=True, help="confirmed ESP32-S3 port, e.g. /dev/ttyACM3"
    )
    parser.add_argument(
        "--baud", type=int, default=2_000_000, help="UART baud (CONFIG_DEMO_UART_BAUD)"
    )
    parser.add_argument("--source", choices=["live", "npz"], default="live")
    parser.add_argument("--npz", type=Path, help="capture for --source npz")
    parser.add_argument(
        "--scenario", default="freedoom1", help="ViZDoom scenario for live"
    )
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument(
        "--style",
        choices=sorted(STYLE_CODES),
        default="outline",
        help="stage 3 on the ESP32",
    )
    parser.add_argument(
        "--fast-i2c", action="store_true", help="1 MHz I2C (out of spec)"
    )
    parser.add_argument("--black", type=int, default=16)
    parser.add_argument("--white", type=int, default=170)
    parser.add_argument("--gamma", type=int, default=100)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if args.frames <= 0:
        parser.error("--frames must be positive")
    if args.source == "npz" and args.npz is None:
        parser.error("--source npz needs --npz")

    if args.source == "npz":
        palette, frames = npz_frames(args.npz, args.frames)
    else:
        palette, frames = live_frames(args.scenario, args.frames, args.seed)
    pipe = OledPipeline()
    pipe.set_palette(palette, args.black, args.white, args.gamma)
    flags = STYLE_CODES[args.style] | (FLAG_FAST_I2C if args.fast_i2c else 0)

    try:
        port = open_port(args.port, args.baud)
    except serial.SerialException as e:
        print(f"error: cannot open {args.port}: {e}", file=sys.stderr)
        return 2
    try:
        return run(args, port, frames, pipe, flags)
    except PortBusyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        port.close()


def run(
    args: argparse.Namespace,
    port: serial.Serial,
    frames: Iterator[np.ndarray],
    pipe: OledPipeline,
    flags: int,
) -> int:
    reader = ReplyReader(port)
    hello = reader.read(3.0)  # device re-announces every second while idle
    if hello is None:
        print(
            "error: no hello from device within 3 s. Check: firmware flashed, right port, "
            f"baud {args.baud} matches CONFIG_DEMO_UART_BAUD, no other program on the port.",
            file=sys.stderr,
        )
        return 2
    else:
        print(f"device ready (status {STATUS_NAMES.get(hello.status, hello.status)})")

    stats = Stats()
    last: Reply | None = None
    t_start = time.monotonic()
    sent = 0
    for seq, frame in enumerate(frames, start=1):
        t0 = time.perf_counter()
        gray = pipe.downscale(frame)
        t1 = time.perf_counter()
        port.write(encode_frame(seq, flags, gray))
        sent += 1
        reply = reader.read(REPLY_TIMEOUT_S)
        while reply is not None and reply.status == STATUS_HELLO:
            reply = reader.read(REPLY_TIMEOUT_S)
        t2 = time.perf_counter()
        if reply is None or reply.seq != (seq & 0xFFFF):
            stats.timeouts += 1
            continue
        if reply.status != STATUS_OK:
            stats.device_errors += 1
        stats.host_ms.append((t1 - t0) * 1e3)
        stats.rtt_ms.append((t2 - t1) * 1e3)
        stats.dither_us.append(reply.dither_us)
        stats.pack_us.append(reply.pack_us)
        stats.i2c_us.append(reply.i2c_us)
        last = reply
    wall = time.monotonic() - t_start

    ok = len(stats.rtt_ms)
    print(
        f"source {args.source}, style {args.style}, "
        f"i2c {'1 MHz' if args.fast_i2c else '400 kHz'}, levels {args.black}..{args.white}"
    )
    print(
        f"frames sent {sent}, replied {ok}, timeouts {stats.timeouts}, "
        f"device errors {stats.device_errors}, bad replies {reader.bad_replies}"
    )
    if last is not None:
        print(
            f"device parser: crc errors {last.crc_errors}, length errors {last.len_errors}"
        )
    print(
        f"achieved {ok / wall:.2f} frames/s over {wall:.1f} s (includes ViZDoom time if live)"
    )
    print(summarize("host stage 1+2", stats.host_ms, "ms"))
    print(summarize("round trip (send..reply)", stats.rtt_ms, "ms"))
    print(summarize("device dither", [v / 1e3 for v in stats.dither_us], "ms"))
    print(summarize("device pack", [v / 1e3 for v in stats.pack_us], "ms"))
    print(summarize("device I2C frame write", [v / 1e3 for v in stats.i2c_us], "ms"))
    return 0 if ok == sent and stats.device_errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
