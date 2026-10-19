"""End-to-end test of the STM32 backend client against the host brain simulator.

The simulator is the real C parser + brain server + rate model behind pipes, so this checks
the Python client, the wire format and C/Python numeric parity without hardware.
"""

from __future__ import annotations

import os
import select
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from game import brain_protocol as bp
from game import stm32_backend as backend
from game.controller import PythonController, decode_action, measure_rest
from model.rate_model import load_graph

ROOT = Path(__file__).resolve().parent.parent
GRAPH = ROOT / "connectome" / "data" / "malecns_v1_0_subgraph.json"
FW = ROOT / "firmware"

pytestmark = pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not available")


class PipeLink:
    """Link over a subprocess's stdin/stdout."""

    def __init__(self, proc: subprocess.Popen) -> None:
        self._proc = proc
        self._fd = proc.stdout.fileno()

    def write(self, data: bytes) -> None:
        self._proc.stdin.write(data)
        self._proc.stdin.flush()

    def read_byte(self, timeout_s: float) -> int | None:
        ready, _, _ = select.select([self._fd], [], [], max(timeout_s, 0.0))
        if not ready:
            return None
        data = os.read(self._fd, 1)
        return data[0] if data else None

    def discard_input(self) -> None:
        while select.select([self._fd], [], [], 0)[0]:
            if not os.read(self._fd, 4096):
                return


@pytest.fixture(scope="module")
def sim_exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    exe = tmp_path_factory.mktemp("sim") / "brain_sim"
    sources = [ROOT / "tests/c/brain_sim.c"] + [
        FW / "src" / name
        for name in (
            "brain_server.c",
            "brain_protocol.c",
            "rate_model.c",
            "plasticity.c",
        )
    ]
    subprocess.run(
        [
            "gcc",
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{FW / 'src'}",
            f"-I{FW / 'include'}",
            f"-I{FW / 'generated'}",
            *map(str, sources),
            str(FW / "generated/model_weights.c"),
            "-lm",
            "-o",
            str(exe),
        ],
        check=True,
    )
    return exe


@pytest.fixture
def brain(sim_exe: Path):
    proc = subprocess.Popen(
        [str(sim_exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )
    try:
        yield backend.Stm32Controller(PipeLink(proc))
    finally:
        proc.stdin.close()
        proc.wait(timeout=5)


def test_ping_reports_model_parameters(brain: backend.Stm32Controller) -> None:
    info = brain.ping()
    assert info.proto_version == bp.PROTOCOL_VERSION
    assert info.n_features == bp.N_FEATURES
    assert info.n_neurons == 300
    assert info.substeps == 6
    assert info.n_learnable == 320


def test_wire_parity_with_python_model(brain: backend.Stm32Controller) -> None:
    """Same Q15 features through the C server (over the wire) and the Python model."""
    py = PythonController(load_graph(GRAPH))
    rest = measure_rest(py)
    py.reset()
    brain.reset()
    rng = np.random.default_rng(5)
    max_diff = 0.0
    turns = 0
    for i in range(80):
        raw = rng.random(bp.N_FEATURES).astype(np.float32)
        if i % 4 != 0:
            raw[4:] = 0.0  # all six random saturates both sides and almost never turns
        feats = bp.quantize_features(raw)
        want = py.step(feats)
        got = brain.step(feats)
        for a, b in zip(
            (got.steer_left, got.steer_right, got.forward, got.backward),
            (want.steer_left, want.steer_right, want.forward, want.backward),
        ):
            assert abs(a - b) <= 1e-5 + 1e-4 * abs(b)
            max_diff = max(max_diff, abs(a - b))
        turn_left, turn_right = decode_action(want, rest)
        expected = (bp.ACTION_TURN_LEFT if turn_left else 0) | (
            bp.ACTION_TURN_RIGHT if turn_right else 0
        )
        assert brain.last_actions == expected
        turns += int(turn_left or turn_right)
    assert max_diff < 1e-4
    assert turns > 5  # the comparison really exercised both turn directions


def test_reset_restarts_dynamics(brain: backend.Stm32Controller) -> None:
    feats = np.array([0.3, 0.1, 0.6, 0.0, 0.2, 0.0], dtype=np.float32)
    first = brain.step(feats)
    for _ in range(10):
        brain.step(feats)
    brain.reset()
    again = brain.step(feats)
    assert again == first


def test_error_reply_raises(brain: backend.Stm32Controller) -> None:
    with pytest.raises(backend.BrainError) as exc:
        brain._transact(0x55)
    assert exc.value.code == bp.ERR_UNKNOWN_TYPE
    with pytest.raises(backend.BrainError) as exc:
        brain._transact(
            bp.MSG_STEP, bytes([0x00, 0x80]) + bytes(bp.STEP_PAYLOAD_BYTES - 2)
        )  # negative Q15
    assert exc.value.code == bp.ERR_BAD_FEATURE
    with pytest.raises(backend.BrainError) as exc:
        brain._transact(bp.MSG_STEP, bytes(8))  # a v1 STEP
    assert exc.value.code == bp.ERR_BAD_LENGTH
    assert brain.ping().n_neurons == 300  # still alive after the errors


def test_timeout_counts_misses_and_raises(monkeypatch: pytest.MonkeyPatch) -> None:
    class DeadLink:
        def write(self, data: bytes) -> None:
            pass

        def read_byte(self, timeout_s: float) -> int | None:
            return None

        def discard_input(self) -> None:
            pass

    monkeypatch.setattr(backend, "REPLY_TIMEOUT_S", 0.01)
    ctl = backend.Stm32Controller(DeadLink())
    with pytest.raises(backend.LinkTimeout):
        ctl.step(np.zeros(bp.N_FEATURES, dtype=np.float32))
    assert ctl.misses == backend.RETRIES + 1


def test_learning_changes_weights_and_reset_weights_restores(
    brain: backend.Stm32Controller,
) -> None:
    """LEARN with reward moves the weights (drift > 0); RESET keeps them; RESET_WEIGHTS
    returns to the connectome (drift 0, same outputs as before learning)."""
    feats = np.array([0.2, 0.7, 0.0, 0.8, 0.0, 0.5], dtype=np.float32)
    brain.reset()
    before = brain.step(feats, flags=bp.STEP_FLAG_RESET_WEIGHTS)
    assert brain.last_drift == 0.0
    for i in range(60):
        brain.step(feats, reward=1.0 if i % 4 == 0 else -0.2, flags=bp.STEP_FLAG_LEARN)
    learned = brain.last_drift
    assert learned > 0.0
    brain.reset()
    brain.step(feats)
    assert brain.last_drift == learned  # RESET does not touch weights; no LEARN: frozen
    brain.reset()
    again = brain.step(feats, flags=bp.STEP_FLAG_RESET_WEIGHTS)
    assert brain.last_drift == 0.0
    assert again == before
