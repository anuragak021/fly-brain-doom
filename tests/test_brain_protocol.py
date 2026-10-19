"""Host tests for the Python brain protocol reference, plus C/Python equivalence."""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from game import brain_protocol as bp

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "firmware" / "include" / "brain_protocol.h"
VECTORS = ROOT / "tests" / "golden" / "protocol_vectors.txt"
PARSER_CLI_SRC = ROOT / "tests" / "c" / "parser_cli.c"
C_SRC = ROOT / "firmware" / "src" / "brain_protocol.c"

# header macro -> python constant. Every wire-relevant number is cross-checked.
HEADER_CONSTANTS = {
    "BRAIN_PROTOCOL_VERSION": bp.PROTOCOL_VERSION,
    "BRAIN_SOF": bp.SOF,
    "BRAIN_HEADER_BYTES": bp.HEADER_BYTES,
    "BRAIN_CRC_BYTES": bp.CRC_BYTES,
    "BRAIN_MAX_PAYLOAD_BYTES": bp.MAX_PAYLOAD_BYTES,
    "BRAIN_CRC32_CHECK_VALUE": bp.CRC32_CHECK_VALUE,
    "BRAIN_INTERBYTE_TIMEOUT_MS": bp.INTERBYTE_TIMEOUT_MS,
    "BRAIN_MSG_PING": bp.MSG_PING,
    "BRAIN_MSG_STEP": bp.MSG_STEP,
    "BRAIN_MSG_RESET": bp.MSG_RESET,
    "BRAIN_MSG_INFO": bp.MSG_INFO,
    "BRAIN_MSG_STEP_RESP": bp.MSG_STEP_RESP,
    "BRAIN_MSG_RESET_RESP": bp.MSG_RESET_RESP,
    "BRAIN_MSG_ERROR": bp.MSG_ERROR,
    "BRAIN_INFO_PAYLOAD_BYTES": bp.INFO_PAYLOAD_BYTES,
    "BRAIN_N_FEATURES": bp.N_FEATURES,
    "BRAIN_STEP_PAYLOAD_BYTES": bp.STEP_PAYLOAD_BYTES,
    "BRAIN_STEP_RESP_PAYLOAD_BYTES": bp.STEP_RESP_PAYLOAD_BYTES,
    "BRAIN_ERROR_PAYLOAD_BYTES": bp.ERROR_PAYLOAD_BYTES,
    "BRAIN_ERR_UNKNOWN_TYPE": bp.ERR_UNKNOWN_TYPE,
    "BRAIN_ERR_BAD_LENGTH": bp.ERR_BAD_LENGTH,
    "BRAIN_ERR_BAD_VERSION": bp.ERR_BAD_VERSION,
    "BRAIN_ERR_BAD_FEATURE": bp.ERR_BAD_FEATURE,
    "BRAIN_ACTION_RESERVED_MASK": 0xFC,
}


def _header_macros() -> dict[str, int]:
    """Plain integer #defines from the header (expressions are skipped)."""
    out = {}
    for m in re.finditer(
        r"^#define\s+(BRAIN_\w+)\s+(0x[0-9A-Fa-f]+|\d+)U?\b",
        HEADER.read_text(),
        re.MULTILINE,
    ):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def _vector_lines(kind: str) -> list[list[str]]:
    return [
        ln.split()
        for ln in VECTORS.read_text().splitlines()
        if ln.startswith(kind + " ")
    ]


def _frames_from_vectors() -> list[tuple[int, int, bytes, bytes]]:
    out = []
    for _, _name, t, seq, payload, frame in _vector_lines("FRAME"):
        out.append(
            (
                int(t, 16),
                int(seq),
                b"" if payload == "-" else bytes.fromhex(payload),
                bytes.fromhex(frame),
            )
        )
    return out


def _drain(parser: bp.Parser, data: bytes) -> list[bp.Frame]:
    frames = []
    for byte in data:
        frame = parser.push(byte)
        while frame is not None:
            frames.append(frame)
            frame = parser.poll()
    return frames


@pytest.mark.parametrize("name,value", sorted(HEADER_CONSTANTS.items()))
def test_python_constants_match_header(name: str, value: int) -> None:
    assert _header_macros()[name] == value, name


def test_header_action_bits_match() -> None:
    text = HEADER.read_text()
    assert "#define BRAIN_ACTION_TURN_LEFT (1U << 0)" in text
    assert "#define BRAIN_ACTION_TURN_RIGHT (1U << 1)" in text
    assert (bp.ACTION_TURN_LEFT, bp.ACTION_TURN_RIGHT) == (1, 2)


def test_crc32_check_value() -> None:
    assert bp.crc32(b"123456789") == 0xCBF43926


def test_vectors_roundtrip_and_parse() -> None:
    frames = _frames_from_vectors()
    assert len(frames) == 7
    for mtype, seq, payload, wire in frames:
        assert bp.encode_frame(mtype, seq, payload) == wire
        [parsed] = _drain(bp.Parser(), wire)
        assert (parsed.type, parsed.seq, parsed.payload, parsed.version) == (
            mtype,
            seq,
            payload,
            1,
        )


def test_q15_vectors() -> None:
    for _, text, q in _vector_lines("Q15"):
        assert bp.feature_to_q15(float(text)) == int(q)


def test_step_request_response_vectors() -> None:
    [_, f0, f1, f2, f3, hex_payload] = _vector_lines("STEPREQ")[0]
    feats = np.array([float(f0), float(f1), float(f2), float(f3)], dtype=np.float32)
    assert bp.encode_step_request(feats).hex() == hex_payload
    back = bp.decode_step_request(bytes.fromhex(hex_payload))
    assert np.allclose(back, feats, atol=1 / 32767)
    [_, act, cyc, sl, sr, fw, bk, resp_hex] = _vector_lines("STEPRESP")[0]
    resp = bp.StepResponse(
        int(act), int(cyc), float(sl), float(sr), float(fw), float(bk)
    )
    assert bp.encode_step_response(resp).hex() == resp_hex
    assert bp.decode_step_response(bytes.fromhex(resp_hex)) == resp


def test_negative_feature_and_bad_length_rejected() -> None:
    with pytest.raises(bp.ProtocolError):
        bp.decode_step_request(bytes([0x00, 0x80]) + bytes(6))
    with pytest.raises(bp.ProtocolError):
        bp.decode_step_request(bytes(7))
    with pytest.raises(bp.ProtocolError):
        bp.encode_frame(bp.MSG_STEP, 0, bytes(bp.MAX_PAYLOAD_BYTES + 1))


def test_quantize_features_is_idempotent_and_bounded() -> None:
    f = np.array([0.0, 0.123456, 1.0, 1.7], dtype=np.float32)
    q = bp.quantize_features(f)
    assert np.array_equal(q, bp.quantize_features(q))
    assert q.min() >= 0 and q.max() <= 1


def test_garbage_back_to_back_and_sof_in_payload() -> None:
    frames = _frames_from_vectors()
    stream = bytes([0x00, 0x13, 0xFF, 0x42]) + b"".join(f[3] for f in frames)
    assert len(_drain(bp.Parser(), stream)) == 7
    payload = bytes([0xA5, 1, 0xA5, 2, 0xA5, 2, 0xA5, 0])
    [f] = _drain(bp.Parser(), bp.encode_frame(bp.MSG_STEP, 9, payload))
    assert f.payload == payload


def test_oversize_len_and_corrupt_crc_then_good() -> None:
    frames = _frames_from_vectors()
    parser = bp.Parser()
    assert _drain(parser, bytes([0xA5, 1, 2, 1, 0xFF])) == []
    assert len(_drain(parser, frames[0][3])) == 1
    bad = bytearray(frames[2][3])
    bad[bp.HEADER_BYTES] ^= 1
    assert _drain(parser, bytes(bad)) == []
    assert len(_drain(parser, frames[4][3])) == 1


def test_false_sof_swallowing_real_frame_and_reset() -> None:
    frames = _frames_from_vectors()
    stream = bytearray([0xA5, 1, 2, 1, bp.MAX_PAYLOAD_BYTES]) + frames[0][3]
    stream += bytes(bp.MAX_FRAME_BYTES - len(stream))
    [f] = _drain(bp.Parser(), bytes(stream))
    assert f.type == bp.MSG_PING
    parser = bp.Parser()
    assert _drain(parser, frames[2][3][:6]) == []
    parser.reset()
    assert len(_drain(parser, frames[2][3])) == 1


@pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")
def test_c_and_python_parsers_agree_on_fuzzed_streams(tmp_path: Path) -> None:
    exe = tmp_path / "parser_cli"
    subprocess.run(
        [
            "gcc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{HEADER.parent}",
            str(PARSER_CLI_SRC),
            str(C_SRC),
            "-o",
            str(exe),
        ],
        check=True,
    )
    rng = np.random.default_rng(2024)
    good = [f[3] for f in _frames_from_vectors()]
    total = 0
    for _ in range(300):
        pieces = []
        for _ in range(int(rng.integers(1, 12))):
            kind = int(rng.integers(0, 4))
            if kind == 0:
                pieces.append(
                    bytes(
                        rng.integers(0, 256, int(rng.integers(0, 60)), dtype=np.uint8)
                    )
                )
            elif kind == 1:
                pieces.append(good[int(rng.integers(0, len(good)))])
            elif kind == 2:  # a good frame with one random byte flipped
                g = bytearray(good[int(rng.integers(0, len(good)))])
                g[int(rng.integers(0, len(g)))] ^= int(rng.integers(1, 256))
                pieces.append(bytes(g))
            else:  # a truncated good frame
                g = good[int(rng.integers(0, len(good)))]
                pieces.append(g[: int(rng.integers(1, len(g)))])
        stream = b"".join(pieces)
        expected = [
            f"{f.type:02x} {f.seq} {len(f.payload)} {f.payload.hex() or '-'}"
            for f in _drain(bp.Parser(), stream)
        ]
        run = subprocess.run(
            [str(exe)], input=stream.hex(), capture_output=True, text=True, check=True
        )
        assert run.stdout.split("\n")[:-1] == expected
        total += len(expected)
    assert total > 100  # the fuzz really did produce frames to compare
