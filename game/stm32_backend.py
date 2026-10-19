"""STM32 backend: the Controller interface over the brain UART protocol.

The game loop talks to ``Controller``; this class implements it by sending STEP frames to the
brain (real board over the ST-Link VCP, or the host simulator over pipes) and decoding the
response. The protocol is defined in ``firmware/include/brain_protocol.h``.
"""

from __future__ import annotations

import glob
import struct
import time
from dataclasses import dataclass
from typing import Protocol

import numpy as np

from game import brain_protocol as bp
from game.controller import Outputs

STLINK_PORT_GLOB = "/dev/serial/by-id/*STLINK*-if02"
BAUD = 115200
REPLY_TIMEOUT_S = 0.25  # generous: USB-serial latency on a laptop is not deterministic
RETRIES = 2


class LinkTimeout(TimeoutError):
    """No valid response arrived in time (after retries)."""


class BrainError(RuntimeError):
    """The brain answered with an ERROR frame."""

    def __init__(self, code: int, request_type: int) -> None:
        super().__init__(f"brain error {code} for request type {request_type:#04x}")
        self.code = code
        self.request_type = request_type


class Link(Protocol):
    """Minimal byte transport."""

    def write(self, data: bytes) -> None:
        """Send bytes."""

    def read_byte(self, timeout_s: float) -> int | None:
        """Return one byte, or None on timeout."""

    def discard_input(self) -> None:
        """Drop any unread bytes."""


class SerialLink:
    """pyserial transport."""

    def __init__(self, port: str, baud: int = BAUD) -> None:
        import serial

        self._ser = serial.Serial(port, baud, timeout=0.01)

    def write(self, data: bytes) -> None:
        self._ser.write(data)
        self._ser.flush()

    def read_byte(self, timeout_s: float) -> int | None:
        self._ser.timeout = timeout_s
        data = self._ser.read(1)
        return data[0] if data else None

    def discard_input(self) -> None:
        self._ser.reset_input_buffer()

    def close(self) -> None:
        """Close the port."""
        self._ser.close()


def find_stlink_port() -> str:
    """The ST-Link VCP by its stable by-id name (ttyACM numbers move)."""
    matches = sorted(glob.glob(STLINK_PORT_GLOB))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one ST-Link serial port ({STLINK_PORT_GLOB}), found {len(matches)}; "
            "check the USB attach (usbipd) or pass --port"
        )
    return matches[0]


@dataclass(frozen=True)
class Info:
    """Decoded INFO response."""

    proto_version: int
    n_features: int
    n_neurons: int
    substeps: int
    core_clock_khz: int


class Stm32Controller:
    """Controller backend over the brain protocol."""

    def __init__(self, link: Link) -> None:
        self._link = link
        self._parser = bp.Parser()
        self._seq = 0
        self.last_actions = 0
        self.last_cycles = 0
        self.misses = 0
        self.round_trips_s: list[float] = []
        self.cycles: list[int] = []

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def _read_frame(self, seq: int, deadline: float) -> bp.Frame | None:
        """Read until a frame with this seq arrives; stale frames are ignored."""
        while time.monotonic() < deadline:
            byte = self._link.read_byte(max(deadline - time.monotonic(), 0.0))
            if byte is None:
                return None
            frame = self._parser.push(byte)
            while frame is not None:
                if frame.seq == seq:
                    return frame
                frame = self._parser.poll()
        return None

    def _transact(self, msg_type: int, payload: bytes = b"") -> bp.Frame:
        for _attempt in range(RETRIES + 1):
            seq = self._next_seq()
            self._parser.reset()
            self._link.discard_input()
            t0 = time.monotonic()
            self._link.write(bp.encode_frame(msg_type, seq, payload))
            frame = self._read_frame(seq, t0 + REPLY_TIMEOUT_S)
            if frame is not None:
                self.round_trips_s.append(time.monotonic() - t0)
                if frame.type == bp.MSG_ERROR:
                    raise BrainError(frame.payload[0], frame.payload[1])
                return frame
            self.misses += 1
        raise LinkTimeout(
            f"no response to request {msg_type:#04x} after {RETRIES + 1} attempts"
        )

    def ping(self) -> Info:
        """Check the link and read the brain's static parameters."""
        frame = self._transact(bp.MSG_PING)
        if frame.type != bp.MSG_INFO or len(frame.payload) != bp.INFO_PAYLOAD_BYTES:
            raise bp.ProtocolError("malformed INFO response")
        ver, nfeat, neurons, substeps, khz = struct.unpack("<BBHBI", frame.payload)
        return Info(ver, nfeat, neurons, substeps, khz)

    def reset(self) -> None:
        """Zero the neuron state."""
        frame = self._transact(bp.MSG_RESET)
        if frame.type != bp.MSG_RESET_RESP:
            raise bp.ProtocolError("unexpected response to RESET")

    def step(self, features: np.ndarray) -> Outputs:
        """One STEP: features in, pooled output activities out (actions kept in last_actions)."""
        frame = self._transact(bp.MSG_STEP, bp.encode_step_request(features))
        if frame.type != bp.MSG_STEP_RESP:
            raise bp.ProtocolError("unexpected response to STEP")
        resp = bp.decode_step_response(frame.payload)
        self.last_actions = resp.actions
        self.last_cycles = resp.step_cycles
        self.cycles.append(resp.step_cycles)
        return Outputs(resp.steer_left, resp.steer_right, resp.forward, resp.backward)
