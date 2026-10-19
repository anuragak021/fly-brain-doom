"""Controller interface and the Python backend.

The game loop only talks to ``Controller``: features in, output activities out.
A later ``stm32`` backend implements the same interface over UART, so the game
loop does not change. Decoding of activities into game actions is done on the
host (``decode_action``), identically for every backend.

Interface contract:
    step(features: float32[N_FEATURES]) -> Outputs
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

import numpy as np

from game.features import (
    FEATURE_ENEMY_L,
    FEATURE_ENEMY_R,
    FEATURE_LUM_L,
    FEATURE_LUM_R,
    N_FEATURES,
)
from model.rate_model import Graph, RateModel

TONIC_DRIVE = 0.2  # baseline drive to every non-input neuron so the net is active
INPUT_GAIN = 5.0  # scales feature drive into the input neurons
LUM_WEIGHT = 0.2  # luminance counts less than the enemy cue
SETTLE_FRAMES = 15  # frames of zero input used to measure the resting state
TURN_DEADZONE = 0.02  # |steer difference above rest| needed to turn
FORWARD_THRESHOLD = 0.05  # forward activity above rest needed to flag "forward"
# +1 = turn toward the side whose visual input is stronger ("approach");
# -1 = the connectome's natural ipsilateral reading, which turns away ("avoid").
TURN_SIGN = 1


@dataclass(frozen=True)
class Outputs:
    """Mean activity of each output group (what the STM32 will also return)."""

    steer_left: float
    steer_right: float
    forward: float
    backward: float


class Controller(Protocol):
    """Backend-independent controller interface."""

    def reset(self) -> None:
        """Return to the resting state."""

    def step(self, features: np.ndarray) -> Outputs:
        """Run one game frame of dynamics for the given feature vector."""


class PythonController:
    """Rate model running in numpy on the host."""

    def __init__(self, graph: Graph) -> None:
        self._graph = graph
        self._model = RateModel(graph.weights)
        self._in_left = graph.indices("input", "L")
        self._in_right = graph.indices("input", "R")
        self._groups = {
            "steer_left": graph.indices("output", "L", "steer"),
            "steer_right": graph.indices("output", "R", "steer"),
            "forward": graph.indices("output", group="forward"),
            "backward": graph.indices("output", group="backward"),
        }
        self._tonic = np.full(graph.n, TONIC_DRIVE, dtype=np.float32)
        self._tonic[graph.indices("input")] = 0.0

    def reset(self) -> None:
        self._model.reset()

    def _drive(self, features: np.ndarray) -> np.ndarray:
        if features.shape != (N_FEATURES,):
            raise ValueError(f"expected {N_FEATURES} features, got {features.shape}")
        drive = self._tonic.copy()
        left = features[FEATURE_ENEMY_L] + LUM_WEIGHT * features[FEATURE_LUM_L]
        right = features[FEATURE_ENEMY_R] + LUM_WEIGHT * features[FEATURE_LUM_R]
        drive[self._in_left] = INPUT_GAIN * left
        drive[self._in_right] = INPUT_GAIN * right
        return drive

    def step(self, features: np.ndarray) -> Outputs:
        r = self._model.step(self._drive(features))
        pooled = {
            k: float(r[ix].mean()) if len(ix) else 0.0 for k, ix in self._groups.items()
        }
        return Outputs(**pooled)


def decode_action(out: Outputs, rest: Outputs) -> tuple[bool, bool]:
    """Map activities (relative to the resting state) to (turn_left, turn_right).

    Driving one side's visual input lowers that side's steering output, so the
    difference (right - left) rises when the stimulus is on the left.
    """
    diff = (out.steer_right - rest.steer_right) - (out.steer_left - rest.steer_left)
    diff *= TURN_SIGN
    return diff > TURN_DEADZONE, diff < -TURN_DEADZONE


def measure_rest(controller: Controller) -> Outputs:
    """Settle the controller with zero visual input and return its resting outputs."""
    controller.reset()
    zero = np.zeros(N_FEATURES, dtype=np.float32)
    out = controller.step(zero)
    for _ in range(SETTLE_FRAMES):
        out = controller.step(zero)
    return out
