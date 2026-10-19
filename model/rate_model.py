"""Leaky rate model on a connectome subgraph (pure numpy, float32).

    tau * dr/dt = -r + relu(W r + I)        (Euler, step dt)

Weight convention: ``W[post, pre]``; the sign belongs to the presynaptic neuron.
Normalisation: every row is divided by its total absolute input, then the whole
matrix is scaled so the spectral radius of |W| equals ``RHO_TARGET`` (< 1).
That makes the update a contraction for any input, so activity cannot run away.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np

TAU_MS = 20.0
DT_MS = 5.0
ALPHA = np.float32(DT_MS / TAU_MS)  # Euler step fraction, exactly representable
SUBSTEPS_PER_FRAME = 6
RHO_TARGET = 0.9


@dataclass(frozen=True)
class Graph:
    """Neurons and the normalised signed weight matrix W[post, pre]."""

    meta: dict
    neurons: list[dict]
    weights: np.ndarray  # float32, shape (n, n)

    @property
    def n(self) -> int:
        return len(self.neurons)

    def indices(
        self, role: str, side: str | None = None, group: str | None = None
    ) -> np.ndarray:
        """Indices of neurons with the given role (and optionally side and group)."""
        return np.array(
            [
                x["idx"]
                for x in self.neurons
                if x["role"] == role
                and (side is None or x["side"] == side)
                and (group is None or x.get("group", "") == group)
            ],
            dtype=np.int64,
        )


def build_weights(graph: dict) -> np.ndarray:
    """Signed, row-normalised, spectrally scaled W[post, pre] (float32)."""
    n = len(graph["neurons"])
    w = np.zeros((n, n), dtype=np.float64)
    for e in graph["edges"]:
        w[e["post"], e["pre"]] += e["sign"] * e["count"]
    row_sum = np.abs(w).sum(axis=1)
    w[row_sum > 0] /= row_sum[row_sum > 0, None]
    rho = float(np.max(np.abs(np.linalg.eigvals(np.abs(w)))))
    if rho > 0:
        w *= RHO_TARGET / rho
    return w.astype(np.float32)


def load_graph(path: Path) -> Graph:
    """Load a graph JSON (real or placeholder) and build its weight matrix."""
    data = json.loads(Path(path).read_text())
    return Graph(data["meta"], data["neurons"], build_weights(data))


class RateModel:
    """Stateful rate model; one instance holds the activity vector ``r``."""

    def __init__(self, weights: np.ndarray) -> None:
        self.w = np.ascontiguousarray(weights, dtype=np.float32)
        self.r = np.zeros(self.w.shape[0], dtype=np.float32)

    def reset(self) -> None:
        """Set all activities to zero."""
        self.r[:] = 0.0

    def step(self, drive: np.ndarray, substeps: int = SUBSTEPS_PER_FRAME) -> np.ndarray:
        """Advance ``substeps`` Euler steps under constant ``drive``; return r."""
        drive = np.asarray(drive, dtype=np.float32)
        for _ in range(substeps):
            target = np.maximum(self.w @ self.r + drive, np.float32(0.0))
            self.r = self.r + ALPHA * (target - self.r)
        return self.r
