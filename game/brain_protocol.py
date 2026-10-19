"""Python reference implementation of the brain UART protocol v1.

The normative definition is ``firmware/include/brain_protocol.h`` (and the README section
"Brain protocol"). This module mirrors it byte for byte and is used by the laptop
``--backend stm32`` client and by the tests. Constants are checked against the header in
``tests/test_brain_protocol.py`` so the two cannot drift apart silently.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

import numpy as np

PROTOCOL_VERSION = 1
SOF = 0xA5
HEADER_BYTES = 5
CRC_BYTES = 4
OVERHEAD_BYTES = HEADER_BYTES + CRC_BYTES
MAX_PAYLOAD_BYTES = 32
MAX_FRAME_BYTES = OVERHEAD_BYTES + MAX_PAYLOAD_BYTES
CRC32_CHECK_VALUE = 0xCBF43926
INTERBYTE_TIMEOUT_MS = 20

MSG_PING = 0x01
MSG_STEP = 0x02
MSG_RESET = 0x03
MSG_INFO = 0x81
MSG_STEP_RESP = 0x82
MSG_RESET_RESP = 0x83
MSG_ERROR = 0xFF

ERR_UNKNOWN_TYPE = 1
ERR_BAD_LENGTH = 2
ERR_BAD_VERSION = 3
ERR_BAD_FEATURE = 4

N_FEATURES = 4
STEP_PAYLOAD_BYTES = 2 * N_FEATURES
STEP_RESP_PAYLOAD_BYTES = 22
INFO_PAYLOAD_BYTES = 9
ERROR_PAYLOAD_BYTES = 2
Q15_MAX = 32767

ACTION_TURN_LEFT = 1 << 0
ACTION_TURN_RIGHT = 1 << 1

_F32 = np.float32


class ProtocolError(ValueError):
    """Raised for malformed payloads or arguments (not for dropped frames)."""


def crc32(data: bytes) -> int:
    """Standard zlib / IEEE 802.3 CRC-32."""
    return zlib.crc32(data) & 0xFFFFFFFF


def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    """Build a complete frame (SOF .. CRC)."""
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ProtocolError(f"payload {len(payload)} bytes exceeds {MAX_PAYLOAD_BYTES}")
    body = bytes([PROTOCOL_VERSION, msg_type, seq & 0xFF, len(payload)]) + payload
    return bytes([SOF]) + body + struct.pack("<I", crc32(body))


@dataclass(frozen=True)
class Frame:
    """A decoded, CRC-checked frame."""

    version: int
    type: int
    seq: int
    payload: bytes


@dataclass
class Parser:
    """Byte-at-a-time parser, behaviourally identical to the C ``brain_parser_t``."""

    _buf: bytearray = field(default_factory=bytearray)

    def reset(self) -> None:
        """Hunt for SOF again (call after an inter-byte timeout)."""
        self._buf.clear()

    def push(self, byte: int) -> Frame | None:
        """Feed one byte; return a frame if one completed. Call poll() afterwards."""
        if not self._buf and byte != SOF:
            return None
        self._buf.append(byte & 0xFF)
        return self.poll()

    def poll(self) -> Frame | None:
        """Process already buffered bytes without a new byte."""
        buf = self._buf
        while buf:
            if buf[0] != SOF:
                self._resync()
                continue
            if len(buf) < HEADER_BYTES:
                return None
            length = buf[4]
            if length > MAX_PAYLOAD_BYTES:
                self._resync()
                continue
            total = OVERHEAD_BYTES + length
            if len(buf) < total:
                return None
            want = struct.unpack_from("<I", buf, HEADER_BYTES + length)[0]
            if crc32(bytes(buf[1 : HEADER_BYTES + length])) != want:
                self._resync()
                continue
            frame = Frame(
                buf[1], buf[2], buf[3], bytes(buf[HEADER_BYTES : HEADER_BYTES + length])
            )
            del buf[:total]
            return frame
        return None

    def _resync(self) -> None:
        """Drop the false SOF at index 0 and everything up to the next SOF."""
        buf = self._buf
        nxt = 1
        while nxt < len(buf) and buf[nxt] != SOF:
            nxt += 1
        del buf[:nxt]


def feature_to_q15(value: float) -> int:
    """float32 evaluation of floor(f * 32767 + 0.5), clamped to 0..32767 (NaN -> 0)."""
    f = _F32(value)
    if not f > 0:
        return 0
    if f >= 1:
        return Q15_MAX
    return int(f * _F32(Q15_MAX) + _F32(0.5))


def feature_from_q15(q15: int) -> np.float32:
    """Q15 -> float32 feature (q / 32767)."""
    return _F32(q15) / _F32(Q15_MAX)


def quantize_features(features: np.ndarray) -> np.ndarray:
    """Round-trip features through Q15, exactly what the receiving brain sees."""
    return np.array(
        [feature_from_q15(feature_to_q15(float(v))) for v in features], dtype=np.float32
    )


def encode_step_request(features: np.ndarray) -> bytes:
    """STEP request payload: 4 x i16 LE Q15."""
    if len(features) != N_FEATURES:
        raise ProtocolError(f"expected {N_FEATURES} features, got {len(features)}")
    return struct.pack("<4h", *(feature_to_q15(float(v)) for v in features))


def decode_step_request(payload: bytes) -> np.ndarray:
    """Inverse of encode_step_request; raises ProtocolError on bad length or negative Q15."""
    if len(payload) != STEP_PAYLOAD_BYTES:
        raise ProtocolError("bad STEP payload length")
    values = struct.unpack("<4h", payload)
    if any(v < 0 for v in values):
        raise ProtocolError("negative feature")
    return np.array([feature_from_q15(v) for v in values], dtype=np.float32)


@dataclass(frozen=True)
class StepResponse:
    """Decoded STEP response."""

    actions: int
    step_cycles: int
    steer_left: float
    steer_right: float
    forward: float
    backward: float


def encode_step_response(resp: StepResponse) -> bytes:
    """STEP response payload (22 bytes)."""
    return struct.pack(
        "<BBI4f",
        resp.actions,
        0,
        resp.step_cycles,
        resp.steer_left,
        resp.steer_right,
        resp.forward,
        resp.backward,
    )


def decode_step_response(payload: bytes) -> StepResponse:
    """Decode a STEP response payload."""
    if len(payload) != STEP_RESP_PAYLOAD_BYTES:
        raise ProtocolError("bad STEP response length")
    actions, _reserved, cycles, sl, sr, fwd, back = struct.unpack("<BBI4f", payload)
    return StepResponse(actions, cycles, sl, sr, fwd, back)


def encode_info(n_neurons: int, substeps: int, core_clock_khz: int) -> bytes:
    """INFO response payload (9 bytes)."""
    return struct.pack(
        "<BBHBI", PROTOCOL_VERSION, N_FEATURES, n_neurons, substeps, core_clock_khz
    )


def encode_error(code: int, request_type: int) -> bytes:
    """ERROR response payload (2 bytes)."""
    return bytes([code, request_type])
